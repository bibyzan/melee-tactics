/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The browser link (src/pc/link.h): a WebRTC data channel, reliable and
 * ordered, that the page opens through the signaling server before the game
 * starts (link.mjs). The game runs on the page's main thread, so these calls
 * reach Module.tacticsLink directly. No link object: offline play. */
#include <emscripten.h>
#include <stddef.h>

#include "monocypher.h"
#include <pc/link.h>

int dht_random_bytes(void* buf, size_t size);

// clang-format off
EM_JS(int, link_web_state, (void), {
  const link = Module.tacticsLink;
  return link ? link.state() : 0;
});
EM_JS(int, link_web_is_host, (void), {
  const link = Module.tacticsLink;
  return link && link.isHost ? 1 : 0;
});
EM_JS(int, link_web_send, (const void* msg, int len), {
  const link = Module.tacticsLink;
  return link ? link.send(HEAPU8.slice(msg, msg + len)) : 0;
});
EM_JS(int, link_web_recv, (void* buf, int cap), {
  const link = Module.tacticsLink;
  const msg = link && link.recv();
  if (!msg) return 0;
  if (msg.length > cap) return -1;
  HEAPU8.set(msg, buf);
  return msg.length;
});
EM_JS(void, link_web_close, (void), {
  const link = Module.tacticsLink;
  if (link) link.close();
});
// clang-format on

PcLinkState pc_link_state(void) {
    switch (link_web_state()) {
    case 1:
        return PC_LINK_CONNECTING;
    case 2:
        return PC_LINK_OPEN;
    case 3:
        return PC_LINK_CLOSED;
    default:
        return PC_LINK_NONE;
    }
}

bool pc_link_is_host(void) {
    return link_web_is_host() != 0;
}

bool pc_link_send(const void* msg, int len) {
    if (len <= 0 || len > PC_LINK_MAX_MESSAGE) {
        return false;
    }
    return link_web_send(msg, len) != 0;
}

int pc_link_recv(void* buf, int cap) {
    int n = link_web_recv(buf, cap);
    return n > 0 ? n : 0;
}

void pc_link_close(void) {
    link_web_close();
}

void pc_link_hash(void* out, int out_len, const void* msg, int len) {
    crypto_blake2b(out, (size_t)out_len, msg, (size_t)len);
}

bool pc_link_random(void* out, int len) {
    return dht_random_bytes(out, (size_t)len) == len;
}
