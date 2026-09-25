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
// The host makes the offer once the guest is in. When the channel opens the
// socket closes, which also takes the lobby off the list: the server is out
// of the loop for the match.

function roomCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  return Array.from(crypto.getRandomValues(new Uint8Array(6)), (b) => alphabet[b % alphabet.length]).join('');
}

function openSession({ room, host, name, iceServers, log }) {
  const inbox = [];
  let state = 1;
  let channel = null;
  const pc = new RTCPeerConnection({ iceServers });
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const query = new URLSearchParams({ room, role: host ? 'host' : 'guest', name: name || '' });
  const ws = new WebSocket(`${scheme}//${location.host}${location.pathname.replace(/[^/]*$/, '')}ws?${query}`);
  const signal = (data) => ws.readyState === WebSocket.OPEN && ws.send(JSON.stringify({ type: 'signal', data }));
  const fail = (why) => {
    if (state !== 3) log(`link: closed (${why})`);
    state = 3;
  };

  function attach(dc) {
    channel = dc;
    dc.binaryType = 'arraybuffer';
    dc.onopen = () => {
      state = 2;
      log('link: open');
      ws.close();
    };
    dc.onmessage = (event) => inbox.push(new Uint8Array(event.data));
    dc.onclose = () => fail('the other player left');
  }

  pc.onicecandidate = (event) => event.candidate && signal({ candidate: event.candidate.toJSON() });
  pc.onconnectionstatechange = () => {
    if (pc.connectionState === 'failed') fail('connection failed');
  };
  if (host) attach(pc.createDataChannel('tactics', { ordered: true }));
  else pc.ondatachannel = (event) => attach(event.channel);

  ws.onmessage = async (event) => {
    const msg = JSON.parse(event.data);
    try {
      if (msg.type === 'peer' && host) {
        await pc.setLocalDescription(await pc.createOffer());
        signal({ description: pc.localDescription.toJSON() });
      } else if (msg.type === 'signal' && msg.data.description) {
        await pc.setRemoteDescription(msg.data.description);
        if (msg.data.description.type === 'offer') {
          await pc.setLocalDescription(await pc.createAnswer());
          signal({ description: pc.localDescription.toJSON() });
        }
      } else if (msg.type === 'signal' && msg.data.candidate) {
        await pc.addIceCandidate(msg.data.candidate);
      } else if (msg.type === 'peer-left' && state === 1) {
        log('link: the other player left the lobby');
      } else if (msg.type === 'error') {
        fail(msg.message);
      }
    } catch (error) {
      log(`link: ${error.message}`);
    }
  };
  ws.onerror = () => state === 1 && fail('cannot reach the server');

  return {
    room,
    host,
    state: () => state,
    send(bytes) {
      if (state !== 2 || channel.readyState !== 'open') return 0;
      channel.send(bytes);
      return 1;
    },
    recv: () => inbox.shift() || null,
    close() {
      channel?.close();
      pc.close();
      ws.close();
      state = 3;
    },
  };
}

export function createLinkManager({ iceServers, log = console.log }) {
  let session = null;
  let list = null;
  let refreshing = false;
  const base = location.pathname.replace(/[^/]*$/, '');

  return {
    get isHost() { return !!session?.host; },
    room: () => session?.room || '',
    state: () => (session ? session.state() : 0),
    host(name) {
      session?.close();
      session = openSession({ room: roomCode(), host: true, name, iceServers, log });
      log(`link: hosting lobby ${session.room}`);
      return 1;
    },
    join(room) {
      session?.close();
      session = openSession({ room, host: false, iceServers, log });
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
