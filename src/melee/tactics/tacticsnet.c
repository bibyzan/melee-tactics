#include "tactics.h"

#include <stdio.h>
#include <string.h>

#include <pc/link.h>
#include <pc/pc.h>

/* A tactics match between two machines. Both run the same simulation from
 * the same seed (tacticssync.c), so the wire carries only the lobby and the
 * picks. The host plays P1 and chooses the seed.
 *
 * A pick stays hidden until both sides have chosen: each side first sends a
 * COMMIT, a BLAKE2b hash of its pick and a random salt, and only once it
 * holds the other side's commit sends the REVEAL. A client that peeks at the
 * wire learns nothing it could still act on. The commit also carries the
 * break's state checksum, so a desync shows at the first break it touches.
 *
 * Messages (all integers little-endian):
 *   HELLO  'H' version:u8 ckind:u8
 *   MATCH  'M' seed:u32 p1_ckind:u8 p2_ckind:u8          host to guest
 *   COMMIT 'C' break:u16 sum:u32 hash:32                  hash of NONE_PICK when
 *   REVEAL 'R' break:u16 pick:u8 salt:16                  not choosing */

enum {
    VERSION = 1,
    NONE_PICK = 0xFF,
    SALT = 16,
    HASH = 32,
};


typedef struct Break {
    int n;
    bool sent_commit, sent_reveal;
    bool got_commit, got_reveal;
    int my_pick, their_pick;
    u8 my_salt[SALT];
    u8 their_hash[HASH];
    u32 my_sum, their_sum;
} Break;

static struct {
    bool on, host, lobby_done, hello_sent, got_hello, desync;
    int my_ckind, their_ckind;
    u32 seed;
    Break brk;
    char status[64];
} N;

static void put32(u8* p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
    p[2] = (u8) (v >> 16);
    p[3] = (u8) (v >> 24);
}

static u32 get32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static void commitHash(u8* out, int brk, int pick, const u8* salt)
{
    u8 msg[3 + SALT];

    msg[0] = (u8) brk;
    msg[1] = (u8) (brk >> 8);
    msg[2] = (u8) pick;
    memcpy(msg + 3, salt, SALT);
    pc_link_hash(out, HASH, msg, sizeof msg);
}

static void setStatus(const char* s)
{
    if (strcmp(N.status, s) != 0) {
        snprintf(N.status, sizeof N.status, "%s", s);
        pc_log_line("tactics-net: %s", s);
    }
}

bool tactics_NetOn(void)
{
    if (!N.on) {
        N.on = pc_link_state() != PC_LINK_NONE;
        if (N.on) {
            N.host = pc_link_is_host();
        }
    }
    return N.on;
}

int tactics_NetLocalPort(void)
{
    return N.host ? 0 : 1;
}

const char* tactics_NetStatus(void)
{
    return N.status;
}

static void sendReveal(void)
{
    u8 m[4 + SALT];

    m[0] = 'R';
    m[1] = (u8) N.brk.n;
    m[2] = (u8) (N.brk.n >> 8);
    m[3] = (u8) N.brk.my_pick;
    memcpy(m + 4, N.brk.my_salt, SALT);
    pc_link_send(m, sizeof m);
    N.brk.sent_reveal = true;
}

static void onMessage(const u8* m, int len)
{
    switch (m[0]) {
    case 'H':
        if (len < 3) {
            return;
        }
        if (m[1] != VERSION) {
            setStatus("The other player runs another version");
            return;
        }
        N.their_ckind = m[2];
        N.got_hello = true;
        return;
    case 'M':
        if (len < 7 || N.host) {
            return;
        }
        N.seed = get32(m + 1);
        N.their_ckind = m[5];
        N.lobby_done = true;
        return;
    case 'C':
        if (len < 7 + HASH || (m[1] | (m[2] << 8)) != N.brk.n) {
            return;
        }
        N.brk.got_commit = true;
        N.brk.their_sum = get32(m + 3);
        memcpy(N.brk.their_hash, m + 7, HASH);
        return;
    case 'R': {
        u8 check[HASH];

        if (len < 4 + SALT || (m[1] | (m[2] << 8)) != N.brk.n || !N.brk.got_commit) {
            return;
        }
        commitHash(check, N.brk.n, m[3], m + 4);
        if (memcmp(check, N.brk.their_hash, HASH) != 0) {
            setStatus("The other player's pick did not match their commit");
            return;
        }
        N.brk.their_pick = m[3] == NONE_PICK ? -1 : m[3];
        N.brk.got_reveal = true;
        return;
    }
    }
}

/* Once per frame, in the draft and in the fight. */
void tactics_NetPoll(void)
{
    u8 m[PC_LINK_MAX_MESSAGE];
    int len;
    PcLinkState s;

    if (!N.on) {
        return;
    }
    s = pc_link_state();
    while ((len = pc_link_recv(m, sizeof m)) > 0) {
        onMessage(m, len);
    }
    if (s == PC_LINK_CLOSED) {
        setStatus("The other player left");
    } else if (s == PC_LINK_CONNECTING) {
        setStatus(N.host ? "Waiting for the other player to join" : "Joining...");
    }
    /* Both commits in: the reveal can go. */
    if (N.brk.sent_commit && N.brk.got_commit && !N.brk.sent_reveal) {
        sendReveal();
    }
}

bool tactics_NetClosed(void)
{
    return N.on && pc_link_state() == PC_LINK_CLOSED;
}

bool tactics_NetLobby(int my_ckind, u32* seed, int* p1_ckind, int* p2_ckind)
{
    tactics_NetPoll();
    if (pc_link_state() != PC_LINK_OPEN) {
        return false;
    }
    if (!N.hello_sent) {
        u8 h[3] = { 'H', VERSION, (u8) my_ckind };

        N.my_ckind = my_ckind;
        pc_link_send(h, sizeof h);
        N.hello_sent = true;
        setStatus(N.host ? "Connected. Waiting for the other player"
                         : "Connected. Waiting for the host");
    }
    tactics_NetPoll();
    /* The host starts the match once both sides are ready, whichever was
     * ready first. */
    if (N.host && N.got_hello && !N.lobby_done) {
        u8 r[7];

        if (!pc_link_random(&N.seed, sizeof N.seed)) {
            setStatus("No secure random source for the seed");
            return false;
        }
        r[0] = 'M';
        put32(r + 1, N.seed);
        r[5] = (u8) N.my_ckind;
        r[6] = (u8) N.their_ckind;
        pc_link_send(r, sizeof r);
        N.lobby_done = true;
    }
    if (!N.lobby_done) {
        return false;
    }
    *seed = N.seed;
    *p1_ckind = N.host ? N.my_ckind : N.their_ckind;
    *p2_ckind = N.host ? N.their_ckind : N.my_ckind;
    setStatus("Match on");
    return true;
}

void tactics_NetBreak(int n)
{
    memset(&N.brk, 0, sizeof N.brk);
    N.brk.n = n;
    N.brk.my_pick = -1;
    N.brk.their_pick = -1;
}

void tactics_NetSendPick(int pick, u32 sum)
{
    u8 m[7 + HASH];
    int wire = pick < 0 ? NONE_PICK : pick;

    if (N.brk.sent_commit) {
        return;
    }
    N.brk.my_pick = wire;
    N.brk.my_sum = sum;
    if (!pc_link_random(N.brk.my_salt, SALT)) {
        setStatus("No secure random source for the commit");
        return;
    }
    m[0] = 'C';
    m[1] = (u8) N.brk.n;
    m[2] = (u8) (N.brk.n >> 8);
    put32(m + 3, sum);
    commitHash(m + 7, N.brk.n, wire, N.brk.my_salt);
    pc_link_send(m, sizeof m);
    N.brk.sent_commit = true;
    tactics_NetPoll();
}

bool tactics_NetTheirPick(int* pick)
{
    tactics_NetPoll();
    if (!N.brk.got_reveal) {
        return false;
    }
    if (N.brk.their_sum != N.brk.my_sum && !N.desync) {
        N.desync = true;
        pc_log_line("tactics-net: DESYNC at break %d: here %08X, there %08X", N.brk.n,
                    N.brk.my_sum, N.brk.their_sum);
        setStatus("Out of sync with the other player");
    }
    *pick = N.brk.their_pick;
    return true;
}

bool tactics_NetWaiting(void)
{
    return N.brk.sent_commit && !N.brk.got_reveal;
}
