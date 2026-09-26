/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_LINK_H
#define PC_LINK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Melee Tactics' pipe to the other player: whole messages, reliable and in
 * order. A tactics match only ever exchanges picks, so this is all it needs;
 * the rollback netplay in net.c is not involved.
 *
 * The game drives it from its own menus: host a lobby, or list the open ones
 * and join one. A session lasts from pc_link_host / pc_link_join to
 * pc_link_close.
 *
 * Browser (platforms/browser/link_web.c): a WebRTC data channel set up
 * through the page server, which also keeps the list of open lobbies.
 * Native (src/pc/link.c): TCP, for testing. Hosting listens on
 * MELEE_LINK_PORT (default 47100); the lobby list is MELEE_LINK_CONNECT
 * (host:port), if set. */

enum {
    PC_LINK_MAX_MESSAGE = 1024,
    PC_LINK_ROOM_LEN = 24,
    PC_LINK_NAME_LEN = 32,
};

typedef enum PcLinkState {
    PC_LINK_NONE,       ///< no session
    PC_LINK_CONNECTING, ///< waiting for the other player
    PC_LINK_OPEN,
    PC_LINK_CLOSED,     ///< the other player left or the link failed
} PcLinkState;

typedef struct PcLinkLobby {
    char room[PC_LINK_ROOM_LEN];
    char name[PC_LINK_NAME_LEN];
} PcLinkLobby;

/* This build can play online at all. */
bool pc_link_available(void);
/* Open a lobby under name; this side hosts (P1). An open lobby is on the
 * list others see; a closed one is joined only through its invite link. */
bool pc_link_host(const char* name, bool open);
/* Join an open lobby from pc_link_lobbies. */
bool pc_link_join(const char* room);
/* Ask for a fresh list of open lobbies. */
void pc_link_refresh(void);
/* The latest list, up to cap entries; -1 until the first list arrives. */
int pc_link_lobbies(PcLinkLobby* out, int cap);
/* The room this side hosts or joined, or "". */
const char* pc_link_room(void);

/* Invites. The link that joins the hosted room (for showing; "" where there
 * is none), and a hand-off to the platform's share sheet or clipboard, true
 * when it took it. A game opened from an invite link reports the room once
 * through pc_link_invite, and the game joins it. */
const char* pc_link_invite_url(void);
bool pc_link_share_invite(void);
bool pc_link_invite(char* room, int cap);

/* Services the connection without blocking; call once per frame. */
PcLinkState pc_link_state(void);
/* The host plays P1, picks the seed and starts the match. */
bool pc_link_is_host(void);
/* False when the message cannot be queued (not open, or too long). */
bool pc_link_send(const void* msg, int len);
/* One whole message into buf, returning its length, or 0 when none. */
int pc_link_recv(void* buf, int cap);
/* End the session; pc_link_state is PC_LINK_NONE after. */
void pc_link_close(void);

/* For the picks' commit and reveal: BLAKE2b, and bytes from the platform's
 * CSPRNG (false when it has none, which fails the commit rather than making
 * a guessable one). */
void pc_link_hash(void* out, int out_len, const void* msg, int len);
bool pc_link_random(void* out, int len);

#ifdef __cplusplus
}
#endif

#endif
