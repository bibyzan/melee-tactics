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
 * Native (src/pc/link.c): TCP. MELEE_LINK_LISTEN=<port> hosts,
 * MELEE_LINK_CONNECT=<host:port> joins.
 * Browser (platforms/browser/link_web.c): a WebRTC data channel the page
 * opens through the signaling server; the page says which side hosts. */

enum {
    PC_LINK_MAX_MESSAGE = 1024,
};

typedef enum PcLinkState {
    PC_LINK_NONE,       ///< not asked for: play offline
    PC_LINK_CONNECTING, ///< waiting for the other player
    PC_LINK_OPEN,
    PC_LINK_CLOSED,     ///< the other player left or the link failed
} PcLinkState;

/* Services the connection without blocking; call once per frame. */
PcLinkState pc_link_state(void);
/* The host plays P1, picks the seed and starts the match. */
bool pc_link_is_host(void);
/* False when the message cannot be queued (not open, or too long). */
bool pc_link_send(const void* msg, int len);
/* One whole message into buf, returning its length, or 0 when none. */
int pc_link_recv(void* buf, int cap);
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
