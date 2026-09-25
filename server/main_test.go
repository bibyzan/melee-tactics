// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
)

func dial(t *testing.T, ctx context.Context, url, room, role string) *websocket.Conn {
	t.Helper()
	c, _, err := websocket.Dial(ctx, url+"/ws?room="+room+"&role="+role, nil)
	if err != nil {
		t.Fatalf("dial %s: %v", role, err)
	}
	return c
}

func read(t *testing.T, ctx context.Context, c *websocket.Conn) message {
	t.Helper()
	_, b, err := c.Read(ctx)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	var m message
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("decode %q: %v", b, err)
	}
	return m
}

func TestRoom(t *testing.T) {
	h := &hub{rooms: map[string]*room{}}
	srv := httptest.NewServer(isolated(httpMux(h)))
	defer srv.Close()
	url := "ws" + strings.TrimPrefix(srv.URL, "http")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	host := dial(t, ctx, url, "ABCD23", "host")
	guest := dial(t, ctx, url, "ABCD23", "guest")
	if m := read(t, ctx, host); m.Type != "peer" {
		t.Fatalf("host got %q, want peer", m.Type)
	}
	if m := read(t, ctx, guest); m.Type != "peer" {
		t.Fatalf("guest got %q, want peer", m.Type)
	}

	// Signaling relays both ways, and nothing but signaling does.
	host.Write(ctx, websocket.MessageText, []byte(`{"type":"chat","data":{"x":1}}`))
	host.Write(ctx, websocket.MessageText, []byte(`{"type":"signal","data":{"description":{"type":"offer"}}}`))
	if m := read(t, ctx, guest); m.Type != "signal" || !strings.Contains(string(m.Data), "offer") {
		t.Fatalf("guest got %+v, want the offer", m)
	}
	guest.Write(ctx, websocket.MessageText, []byte(`{"type":"signal","data":{"candidate":{"candidate":"c"}}}`))
	if m := read(t, ctx, host); m.Type != "signal" || !strings.Contains(string(m.Data), "candidate") {
		t.Fatalf("host got %+v, want the candidate", m)
	}

	// A second host is turned away.
	extra := dial(t, ctx, url, "ABCD23", "host")
	if m := read(t, ctx, extra); m.Type != "error" {
		t.Fatalf("second host got %q, want error", m.Type)
	}

	// Leaving tells the other side.
	guest.Close(websocket.StatusNormalClosure, "")
	if m := read(t, ctx, host); m.Type != "peer-left" {
		t.Fatalf("host got %q, want peer-left", m.Type)
	}
}

func TestIsolationHeaders(t *testing.T) {
	srv := httptest.NewServer(isolated(httpMux(&hub{rooms: map[string]*room{}})))
	defer srv.Close()
	res, err := srv.Client().Get(srv.URL + "/config")
	if err != nil {
		t.Fatal(err)
	}
	if res.Header.Get("Cross-Origin-Opener-Policy") != "same-origin" ||
		res.Header.Get("Cross-Origin-Embedder-Policy") != "require-corp" {
		t.Fatalf("missing isolation headers: %v", res.Header)
	}
}
