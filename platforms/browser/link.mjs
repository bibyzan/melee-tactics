// SPDX-License-Identifier: GPL-3.0-or-later
// Melee Tactics' link to the other player, driven from the game's own menus
// (link_web.c polls this as Module.tacticsLink):
//   host(name)  open a lobby others can find; this side is P1
//   join(room)  join one from lobbies()
//   refresh()   fetch the open lobbies; lobbies() is the latest list or null
//   state()     0 none, 1 connecting, 2 open, 3 closed
//   send(u8)    queue one message, 1 on success;  recv() -> Uint8Array|null
//   close()     end the session
//
// A session is a WebRTC data channel (reliable, ordered) opened through the
// page server's signaling socket, /ws?room=CODE&role=host|guest&name=...:
//   server -> {type:'peer'}         the other side is in the room
//   either -> {type:'signal', data} an SDP description or an ICE candidate
//   server -> {type:'peer-left'} / {type:'error', message}
// The host makes the offer once the guest is in. The socket stays open: if
// no direct channel opens within DIRECT_WAIT_MS, or it drops, either side
// sends {type:'use-relay'} and game messages go through the server as
// {type:'relay', data:base64}. Phones on cellular often need that. A lobby
// leaves the list once its guest is in.

function roomCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  return Array.from(crypto.getRandomValues(new Uint8Array(6)), (b) => alphabet[b % alphabet.length]).join('');
}

// Game messages over the signaling socket, when the browsers cannot reach
// each other directly: base64 in {type:'relay', data}.
const toBase64 = (bytes) => btoa(String.fromCharCode(...bytes));
const fromBase64 = (text) => Uint8Array.from(atob(text), (c) => c.charCodeAt(0));

// How long both players can be in the room before the direct connection
// counts as blocked and the session goes through the server instead.
const DIRECT_WAIT_MS = 8000;

function openSession({ room, host, name, open = true, iceServers, log, forceRelay }) {
  const inbox = [];
  let state = 1;
  let relay = false;
  let peerGone = false;
  let channel = null;
  let directTimer = null;
  const pc = new RTCPeerConnection({ iceServers });
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const query = new URLSearchParams({ room, role: host ? 'host' : 'guest', name: name || '', open: open ? '1' : '0' });
  // The socket stays open for the whole session: it carries the fallback.
  const ws = new WebSocket(`${scheme}//${location.host}${location.pathname.replace(/[^/]*$/, '')}ws?${query}`);
  const post = (msg) => ws.readyState === WebSocket.OPEN && ws.send(JSON.stringify(msg));
  const signal = (data) => post({ type: 'signal', data });
  const fail = (why) => {
    if (state !== 3) log(`link: closed (${why})`);
    state = 3;
    clearTimeout(directTimer);
  };

  // Both sides end up here, whichever noticed first: game messages then go
  // through the server, which phones on cellular networks often need.
  function useRelay(tell, why) {
    if (relay || state === 3) return;
    if (ws.readyState !== WebSocket.OPEN) {
      fail(why);
      return;
    }
    relay = true;
    clearTimeout(directTimer);
    if (tell) post({ type: 'use-relay' });
    log(`link: going through the server (${why})`);
    state = 2;
    try {
      channel?.close();
      pc.close();
    } catch {}
  }

  function waitForDirect() {
    clearTimeout(directTimer);
    directTimer = setTimeout(() => state === 1 && useRelay(true, 'no direct connection'), DIRECT_WAIT_MS);
  }

  function attach(dc) {
    channel = dc;
    dc.binaryType = 'arraybuffer';
    dc.onopen = () => {
      if (relay) return;
      clearTimeout(directTimer);
      state = 2;
      log('link: open (direct)');
    };
    dc.onmessage = (event) => inbox.push(new Uint8Array(event.data));
    dc.onclose = () => {
      if (relay) return;
      if (peerGone) fail('the other player left');
      else useRelay(true, 'the direct connection dropped');
    };
  }

  pc.onicecandidate = (event) => event.candidate && signal({ candidate: event.candidate.toJSON() });
  pc.onconnectionstatechange = () => {
    if (relay) return;
    if (pc.connectionState === 'failed' || (pc.connectionState === 'disconnected' && state === 2)) {
      useRelay(true, `direct connection ${pc.connectionState}`);
    }
  };
  if (host) attach(pc.createDataChannel('tactics', { ordered: true }));
  else pc.ondatachannel = (event) => attach(event.channel);

  ws.onmessage = async (event) => {
    const msg = JSON.parse(event.data);
    try {
      if (msg.type === 'peer' && forceRelay) {
        useRelay(true, 'asked for with ?relay=1');
      } else if (msg.type === 'peer') {
        waitForDirect();
        if (host) {
          await pc.setLocalDescription(await pc.createOffer());
          signal({ description: pc.localDescription.toJSON() });
        }
      } else if (msg.type === 'relay') {
        inbox.push(fromBase64(msg.data));
      } else if (msg.type === 'use-relay') {
        useRelay(false, 'the other player asked');
      } else if (msg.type === 'signal' && msg.data.description && !relay) {
        await pc.setRemoteDescription(msg.data.description);
        if (msg.data.description.type === 'offer') {
          await pc.setLocalDescription(await pc.createAnswer());
          signal({ description: pc.localDescription.toJSON() });
        }
      } else if (msg.type === 'signal' && msg.data.candidate && !relay) {
        await pc.addIceCandidate(msg.data.candidate);
      } else if (msg.type === 'peer-left') {
        peerGone = true;
        if (relay || state === 1) fail('the other player left');
      } else if (msg.type === 'error') {
        fail(msg.message);
      }
    } catch (error) {
      log(`link: ${error.message}`);
    }
  };
  ws.onerror = () => state === 1 && fail('cannot reach the server');
  ws.onclose = () => {
    if (relay) fail('lost the server');
  };

  return {
    room,
    host,
    open,
    state: () => state,
    send(bytes) {
      if (state !== 2) return 0;
      if (relay) return post({ type: 'relay', data: toBase64(bytes) }) ? 1 : 0;
      if (channel?.readyState !== 'open') return 0;
      channel.send(bytes);
      return 1;
    },
    recv: () => inbox.shift() || null,
    close() {
      clearTimeout(directTimer);
      try {
        channel?.close();
        pc.close();
      } catch {}
      ws.close();
      state = 3;
    },
  };
}

// A lobby's invite link opens the page with ?join=ROOM; see takeInvite.
export function inviteFromUrl(params) {
  const room = (params.get('join') || '').toUpperCase();
  return /^[A-Z0-9]{4,12}$/.test(room) ? room : '';
}

export function createLinkManager({ iceServers, log = console.log, forceRelay = false, invite = '',
                                   onCopied = () => {} }) {
  let session = null;
  let list = null;
  let refreshing = false;
  let pendingInvite = invite;
  const base = location.pathname.replace(/[^/]*$/, '');
  const inviteUrl = () =>
    (session?.host ? `${location.origin}${base}?join=${session.room}` : '');

  return {
    get isHost() { return !!session?.host; },
    room: () => session?.room || '',
    state: () => (session ? session.state() : 0),
    host(name, open = true) {
      session?.close();
      session = openSession({ room: roomCode(), host: true, name, open, iceServers, log, forceRelay });
      log(`link: hosting ${open ? 'an open' : 'a closed'} lobby ${session.room}`);
      return 1;
    },
    inviteUrl,
    // A closed lobby still waiting for its friend: the page offers its own
    // share button then.
    waitingForFriend: () => !!session?.host && !session.open && session.state() === 1,
    // The phone's share sheet, else the clipboard. Called on a button press
    // in the game, which counts as the user's gesture for both.
    shareInvite() {
      const url = inviteUrl();
      if (!url) return 0;
      const text = 'Play me in Melee Tactics! Tap to join my lobby:';
      const copy = () => navigator.clipboard?.writeText(url)
        .then(() => { log(`link: copied ${url}`); onCopied(); }, () => {});
      if (navigator.share) {
        // Refused (no recent tap to count as the gesture): copy it instead.
        navigator.share({ title: 'Melee Tactics', text, url })
          .catch((error) => error?.name !== 'AbortError' && copy());
        return 1;
      }
      if (navigator.clipboard?.writeText) {
        copy();
        return 1;
      }
      return 0;
    },
    // The lobby an invite link asked for, once: the game joins it.
    takeInvite() {
      const room = pendingInvite;
      pendingInvite = '';
      if (room) {
        const url = new URL(location.href);
        url.searchParams.delete('join');
        history.replaceState(null, '', url);
      }
      return room;
    },
    join(room) {
      session?.close();
      session = openSession({ room, host: false, iceServers, log, forceRelay });
      log(`link: joining lobby ${room}`);
      return 1;
    },
    close() {
      session?.close();
      session = null;
    },
    send: (bytes) => (session ? session.send(bytes) : 0),
    recv: () => (session ? session.recv() : null),
    refresh() {
      if (refreshing) return;
      refreshing = true;
      fetch(`${base}lobbies`, { cache: 'no-store' })
        .then((res) => res.json())
        .then((lobbies) => { list = lobbies.filter((l) => l.room !== session?.room); })
        .catch(() => { list = []; })
        .finally(() => { refreshing = false; });
    },
    lobbies: () => list,
  };
}
