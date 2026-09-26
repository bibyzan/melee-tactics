/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The browser link (src/pc/link.h), driven by the game's menus: the page's
 * link manager (link.mjs, Module.tacticsLink) opens a WebRTC data channel
 * through the page server, and lists that server's open lobbies. The game
 * runs on the page's main thread, so these calls reach it directly. */
#include <emscripten.h>
#include <stddef.h>
#include <stdio.h>

#include "monocypher.h"
#include <pc/link.h>

int dht_random_bytes(void* buf, size_t size);

// clang-format off
EM_JS(int, link_web_available, (void), {
  return Module.tacticsLink ? 1 : 0;
});
EM_JS(int, link_web_host, (const char* name, int open), {
  return Module.tacticsLink ? Module.tacticsLink.host(UTF8ToString(name), !!open) : 0;
});
EM_JS(void, link_web_invite_url, (char* out, int len), {
  stringToUTF8(Module.tacticsLink ? Module.tacticsLink.inviteUrl() : '', out, len);
});
EM_JS(int, link_web_share_invite, (void), {
  return Module.tacticsLink ? Module.tacticsLink.shareInvite() : 0;
});
EM_JS(int, link_web_invite, (char* out, int len), {
  const room = Module.tacticsLink ? Module.tacticsLink.takeInvite() : '';
  if (!room) return 0;
  stringToUTF8(room, out, len);
  return 1;
});
EM_JS(int, link_web_join, (const char* room), {
  return Module.tacticsLink ? Module.tacticsLink.join(UTF8ToString(room)) : 0;
});
EM_JS(void, link_web_refresh, (void), {
  if (Module.tacticsLink) Module.tacticsLink.refresh();
});
EM_JS(int, link_web_lobby_count, (void), {
  const list = Module.tacticsLink && Module.tacticsLink.lobbies();
  return list ? list.length : -1;
});
EM_JS(void, link_web_lobby, (int i, char* room, int room_len, char* name, int name_len), {
  const lobby = Module.tacticsLink.lobbies()[i];
  stringToUTF8(lobby.room, room, room_len);
  stringToUTF8(lobby.name, name, name_len);
});
EM_JS(void, link_web_room, (char* out, int len), {
  stringToUTF8(Module.tacticsLink ? Module.tacticsLink.room() : '', out, len);
});
EM_JS(int, link_web_state, (void), {
  return Module.tacticsLink ? Module.tacticsLink.state() : 0;
});
EM_JS(int, link_web_is_host, (void), {
  return Module.tacticsLink && Module.tacticsLink.isHost ? 1 : 0;
});
EM_JS(int, link_web_send, (const void* msg, int len), {
  return Module.tacticsLink ? Module.tacticsLink.send(HEAPU8.slice(msg, msg + len)) : 0;
});
EM_JS(int, link_web_recv, (void* buf, int cap), {
  const msg = Module.tacticsLink && Module.tacticsLink.recv();
  if (!msg) return 0;
  if (msg.length > cap) return -1;
  HEAPU8.set(msg, buf);
  return msg.length;
});
EM_JS(void, link_web_close, (void), {
  if (Module.tacticsLink) Module.tacticsLink.close();
});
// clang-format on

bool pc_link_available(void) {
    return link_web_available() != 0;
}

bool pc_link_host(const char* name, bool open) {
    return link_web_host(name, open ? 1 : 0) != 0;
}

const char* pc_link_invite_url(void) {
    static char url[160];

    link_web_invite_url(url, sizeof url);
    return url;
}

bool pc_link_share_invite(void) {
    return link_web_share_invite() != 0;
}

bool pc_link_invite(char* room, int cap) {
    return link_web_invite(room, cap) != 0;
}

bool pc_link_join(const char* room) {
    return link_web_join(room) != 0;
}

void pc_link_refresh(void) {
    link_web_refresh();
}

int pc_link_lobbies(PcLinkLobby* out, int cap) {
    int n = link_web_lobby_count();
    int i;

    if (n < 0) {
        return -1;
    }
    for (i = 0; i < n && i < cap; i++) {
        link_web_lobby(i, out[i].room, sizeof out[i].room, out[i].name, sizeof out[i].name);
    }
    return i;
}

const char* pc_link_room(void) {
    static char room[PC_LINK_ROOM_LEN];

    link_web_room(room, sizeof room);
    return room;
}

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
