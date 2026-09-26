// SPDX-License-Identifier: GPL-3.0-or-later

// Command server hosts Melee Tactics on the web: the browser build and the
// signaling that pairs two players' browsers over WebRTC. Once their data
// channel opens, the match runs peer to peer; this server only introduced
// them. No game data is served: players load their own disc in the page.
//
// Environment:
//
//	PORT         listen port (default 8080)
//	WEB_DIR      the built page (default ./web)
//	TLS_CERT     with TLS_KEY, serve HTTPS; browsers only run the game's
//	TLS_KEY      threads on a secure origin, so another machine on the LAN
//	             needs this (localhost does not)
//	ICE_SERVERS  JSON list handed to the page, for adding TURN; default is a
//	             public STUN server
//	DEV_DISC     local testing only: a disc image the page can stream from
//	             /dev/disc (?dev_disc=1) instead of asking for a file
package main

import (
	"context"
	"encoding/json"
	"errors"
	"html"
	"log"
	"mime"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/coder/websocket"
)

const defaultICE = `[{"urls":"stun:stun.l.google.com:19302"}]`

var roomPattern = regexp.MustCompile(`^[A-Z0-9]{4,12}$`)

// A room holds at most a host and a guest, and relays signaling between them.
// A room with a host and no guest is a lobby; an open one is listed at
// /lobbies, a closed one only reached through its invite link.
type room struct {
	peers   map[string]*peer // "host", "guest"
	name    string           // what the lobby list shows
	closed  bool             // joined only by invite link: not listed
	created time.Time
}

// lobby is one entry of /lobbies.
type lobby struct {
	Room string `json:"room"`
	Name string `json:"name"`
	Age  int    `json:"age"` // seconds open
}

type peer struct {
	conn *websocket.Conn
	send chan []byte
}

type hub struct {
	mu    sync.Mutex
	rooms map[string]*room
}

type message struct {
	Type    string          `json:"type"`
	Data    json.RawMessage `json:"data,omitempty"`
	Message string          `json:"message,omitempty"`
}

func encode(m message) []byte {
	b, _ := json.Marshal(m)
	return b
}

func other(role string) string {
	if role == "host" {
		return "guest"
	}
	return "host"
}

// join puts p in the room, or says why it cannot. A host opens the room; a
// guest can only join one whose host is still there.
func (h *hub) join(code, role, name string, closed bool, p *peer) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := h.rooms[code]
	if r == nil {
		if role == "guest" {
			return errors.New("that lobby is no longer open")
		}
		r = &room{peers: map[string]*peer{}, created: time.Now()}
		h.rooms[code] = r
	}
	if r.peers[role] != nil {
		if role == "guest" {
			return errors.New("that lobby already has an opponent")
		}
		return errors.New("that room already has a host")
	}
	if role == "host" {
		r.name = name
		r.closed = closed
	}
	r.peers[role] = p
	if q := r.peers[other(role)]; q != nil {
		// Both in: the host makes the offer when it hears this.
		q.push(encode(message{Type: "peer"}))
		p.push(encode(message{Type: "peer"}))
	}
	return nil
}

func (h *hub) leave(code, role string, p *peer) {
	h.mu.Lock()
	defer h.mu.Unlock()
	r := h.rooms[code]
	if r == nil || r.peers[role] != p {
		return
	}
	delete(r.peers, role)
	if q := r.peers[other(role)]; q != nil {
		q.push(encode(message{Type: "peer-left"}))
	}
	if len(r.peers) == 0 {
		delete(h.rooms, code)
	}
}

// lobbies lists the rooms waiting for an opponent, newest first.
func (h *hub) lobbies() []lobby {
	h.mu.Lock()
	defer h.mu.Unlock()
	now := time.Now()
	list := []lobby{}
	for code, r := range h.rooms {
		if r.peers["host"] != nil && r.peers["guest"] == nil && !r.closed {
			list = append(list, lobby{Room: code, Name: r.name, Age: int(now.Sub(r.created).Seconds())})
		}
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Age < list[j].Age })
	if len(list) > 50 {
		list = list[:50]
	}
	return list
}

// lobbyName keeps what a host calls its lobby short and printable.
func lobbyName(s, code string) string {
	var b strings.Builder
	for _, c := range s {
		if b.Len() >= 24 {
			break
		}
		if c >= ' ' && c <= '~' {
			b.WriteRune(c)
		}
	}
	if name := strings.TrimSpace(b.String()); name != "" {
		return name
	}
	return "Lobby " + code
}

func (h *hub) relay(code, role string, m []byte) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if r := h.rooms[code]; r != nil {
		if q := r.peers[other(role)]; q != nil {
			q.push(m)
		}
	}
}

// push queues without blocking; a peer too slow to drain 64 messages of
// signaling is dropped rather than stalling the room.
func (p *peer) push(b []byte) {
	select {
	case p.send <- b:
	default:
		p.conn.Close(websocket.StatusPolicyViolation, "too slow")
	}
}

func (h *hub) serveWS(w http.ResponseWriter, r *http.Request) {
	code, role := r.URL.Query().Get("room"), r.URL.Query().Get("role")
	if !roomPattern.MatchString(code) || (role != "host" && role != "guest") {
		http.Error(w, "room and role required", http.StatusBadRequest)
		return
	}
	conn, err := websocket.Accept(w, r, nil)
	if err != nil {
		return
	}
	conn.SetReadLimit(64 << 10)
	p := &peer{conn: conn, send: make(chan []byte, 64)}
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	// open=0: a closed lobby, joined only through its invite link.
	closed := r.URL.Query().Get("open") == "0"
	if err := h.join(code, role, lobbyName(r.URL.Query().Get("name"), code), closed, p); err != nil {
		wctx, wcancel := context.WithTimeout(ctx, 5*time.Second)
		conn.Write(wctx, websocket.MessageText, encode(message{Type: "error", Message: err.Error()}))
		wcancel()
		conn.Close(websocket.StatusPolicyViolation, err.Error())
		return
	}
	defer h.leave(code, role, p)
	log.Printf("room %s: %s joined", code, role)

	go func() {
		// A ping every 20 seconds keeps a quiet connection (both players
		// thinking over a pick) from being dropped along the way.
		ping := time.NewTicker(20 * time.Second)
		defer ping.Stop()
		for {
			select {
			case <-ping.C:
				pctx, pcancel := context.WithTimeout(ctx, 10*time.Second)
				err := conn.Ping(pctx)
				pcancel()
				if err != nil {
					cancel()
					return
				}
			case b := <-p.send:
				wctx, wcancel := context.WithTimeout(ctx, 10*time.Second)
				err := conn.Write(wctx, websocket.MessageText, b)
				wcancel()
				if err != nil {
					cancel()
					return
				}
			case <-ctx.Done():
				return
			}
		}
	}()
	for {
		_, b, err := conn.Read(ctx)
		if err != nil {
			log.Printf("room %s: %s left", code, role)
			return
		}
		var m message
		if json.Unmarshal(b, &m) != nil {
			continue
		}
		// Signaling, and the fallback for networks where the browsers cannot
		// reach each other directly (phones on cellular, mostly): "use-relay"
		// switches both sides over, and "relay" carries a game message. Each
		// passes through re-encoded; nothing else a client sends reaches the
		// other side.
		switch m.Type {
		case "signal", "relay":
			h.relay(code, role, encode(message{Type: m.Type, Data: m.Data}))
		case "use-relay":
			h.relay(code, role, encode(message{Type: m.Type}))
		}
	}
}

// isolated adds what the page needs to run threads (SharedArrayBuffer):
// cross-origin isolation.
func isolated(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")
		w.Header().Set("Cross-Origin-Resource-Policy", "same-origin")
		if strings.HasSuffix(r.URL.Path, "/") || strings.HasSuffix(r.URL.Path, ".html") ||
			strings.HasSuffix(r.URL.Path, ".mjs") {
			w.Header().Set("Cache-Control", "no-cache")
		}
		next.ServeHTTP(w, r)
	})
}

func main() {
	mime.AddExtensionType(".wasm", "application/wasm")
	mime.AddExtensionType(".mjs", "text/javascript")
	mime.AddExtensionType(".webmanifest", "application/manifest+json")

	port := envOr("PORT", "8080")
	webDir = envOr("WEB_DIR", webDir)
	iceServers = json.RawMessage(envOr("ICE_SERVERS", defaultICE))
	if !json.Valid(iceServers) {
		log.Fatal("ICE_SERVERS is not valid JSON")
	}
	web := webDir
	devDisc = os.Getenv("DEV_DISC")
	if devDisc != "" {
		log.Printf("DEV_DISC: serving %s at /dev/disc, for local testing only", devDisc)
	}
	h := &hub{rooms: map[string]*room{}}
	srv := &http.Server{Addr: ":" + port, Handler: isolated(httpMux(h)), ReadHeaderTimeout: 10 * time.Second}
	cert, key := os.Getenv("TLS_CERT"), os.Getenv("TLS_KEY")
	if cert != "" && key != "" {
		log.Printf("Melee Tactics on https://0.0.0.0:%s (web %s)", port, web)
		log.Fatal(srv.ListenAndServeTLS(cert, key))
	}
	log.Printf("Melee Tactics on http://0.0.0.0:%s (web %s)", port, web)
	log.Fatal(srv.ListenAndServe())
}

var (
	webDir     = "./web"
	iceServers = json.RawMessage(defaultICE)
	devDisc    string
)

func httpMux(h *hub) *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", h.serveWS)
	mux.HandleFunc("/config", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"iceServers":` + string(iceServers) + `}`))
	})
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok")) })
	mux.HandleFunc("/lobbies", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(h.lobbies())
	})
	if devDisc != "" {
		// Local testing only: the page streams this disc (?dev_disc=1)
		// instead of asking for a file in every tab. Never set in a
		// deployment.
		mux.HandleFunc("/dev/disc", func(w http.ResponseWriter, r *http.Request) {
			http.ServeFile(w, r, devDisc)
		})
	}
	files := http.FileServer(http.Dir(webDir))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/" || r.URL.Path == "/index.html" {
			servePage(w, r)
			return
		}
		files.ServeHTTP(w, r)
	})
	return mux
}

// servePage is index.html with its link preview filled in: a texted invite
// link shows as a card with the icon, which needs absolute URLs.
func servePage(w http.ResponseWriter, r *http.Request) {
	page, err := os.ReadFile(filepath.Join(webDir, "index.html"))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	origin := scheme + "://" + r.Host
	title, about := "Melee Tactics", "Melee as a game of reads: pick your moves, then watch them play out."
	if r.URL.Query().Get("join") != "" {
		title, about = "Join my Melee Tactics lobby", "You're invited to a match. Tap to join."
	}
	tags := `<meta property="og:type" content="website">` +
		`<meta property="og:title" content="` + html.EscapeString(title) + `">` +
		`<meta property="og:description" content="` + html.EscapeString(about) + `">` +
		`<meta property="og:image" content="` + html.EscapeString(origin) + `/icons/icon-512.png">` +
		`<meta property="og:url" content="` + html.EscapeString(origin+r.URL.RequestURI()) + `">`
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Write([]byte(strings.Replace(string(page), "<!--og-->", tags, 1)))
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
