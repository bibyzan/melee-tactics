// SPDX-License-Identifier: GPL-3.0-or-later
// Melee Tactics' link to the other player: a WebRTC data channel (reliable,
// ordered) opened through the page server's signaling socket. The engine
// polls it from link_web.c as Module.tacticsLink:
//   state()  0 none, 1 connecting, 2 open, 3 closed
//   isHost   the host plays P1 and picks the seed
//   send(u8) queue one message, 1 on success
//   recv()   the next whole message as a Uint8Array, or null
//
// Signaling (JSON over /ws?room=CODE&role=host|guest), relayed as is:
//   server -> {type:'peer'}         the other side is in the room
//   either -> {type:'signal', data} an SDP description or an ICE candidate
//   server -> {type:'peer-left'} / {type:'error', message}
// The host makes the offer once the guest is in. After the channel opens the
// socket closes: the server is out of the loop for the match.

export function createLink({ room, host, iceServers, log = console.log }) {
  const inbox = [];
  let state = 1;
  let channel = null;
  const pc = new RTCPeerConnection({ iceServers });
  const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${scheme}//${location.host}/ws?room=${encodeURIComponent(room)}&role=${host ? 'host' : 'guest'}`);
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
        log('link: the other player left the room');
      } else if (msg.type === 'error') {
        fail(msg.message);
      }
    } catch (error) {
      log(`link: ${error.message}`);
    }
  };
  ws.onerror = () => state === 1 && fail('cannot reach the signaling server');

  return {
    isHost: host,
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
      fail('closed here');
    },
  };
}
