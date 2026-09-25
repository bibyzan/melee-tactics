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
 *   REVEAL 'R' break:u16 pick:u8 salt:16                  not choosing
 *
 * The link may switch transports mid-session (a direct WebRTC channel that
 * drops falls back to the server), so a message can be lost. Anything still
 * unanswered is sent again every RESEND_POLLS polls, duplicates are ignored,
 * a HELLO after the match is set up gets the MATCH again, a COMMIT for the
 * last break gets that break's REVEAL again, and a COMMIT for the next break
 * that arrives early is kept for it. */

enum {
    RESEND_POLLS = 60,
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
    u8 commit_msg[7 + HASH]; /* as sent, for resending */
    u8 reveal_msg[4 + SALT];
} Break;

static struct {
    bool on, host, lobby_done, hello_sent, got_hello, desync;
    int my_ckind, their_ckind;
    u32 seed;
    Break brk;
    u8 hello_msg[3], match_msg[7];
    /* The last break's reveal, for a peer still waiting on it. */
    int prev_n;
    bool prev_revealed;
    u8 prev_reveal[4 + SALT];
    /* A commit for the next break that came early. */
    bool early;
    u32 early_sum;
    u8 early_hash[HASH];
    int polls;
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
    return N.on;
}

bool tactics_NetAvailable(void)
{
    return pc_link_available();
}

/* A session starts from nothing: the last one's lobby, picks and status are
 * gone. */
static void fresh(bool host)
{
    memset(&N, 0, sizeof N);
    N.on = true;
    N.host = host;
}

bool tactics_NetHost(const char* name)
{
    fresh(true);
    setStatus("Waiting for an opponent to join...");
    if (!pc_link_host(name)) {
        setStatus("Could not open a lobby");
        return false;
    }
    return true;
}

bool tactics_NetJoin(const char* room)
{
    fresh(false);
    setStatus("Joining...");
    if (!pc_link_join(room)) {
        setStatus("Could not join that lobby");
        return false;
    }
    return true;
}

void tactics_NetLeave(void)
{
    if (N.on) {
        pc_link_close();
    }
    memset(&N, 0, sizeof N);
}

/* The other player is connected (the link is open). */
bool tactics_NetConnected(void)
{
    return N.on && pc_link_state() == PC_LINK_OPEN;
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
    u8* m = N.brk.reveal_msg;

    m[0] = 'R';
    m[1] = (u8) N.brk.n;
    m[2] = (u8) (N.brk.n >> 8);
    m[3] = (u8) N.brk.my_pick;
    memcpy(m + 4, N.brk.my_salt, SALT);
    pc_link_send(m, sizeof N.brk.reveal_msg);
    N.brk.sent_reveal = true;
}

static int msgBreak(const u8* m)
{
    return m[1] | (m[2] << 8);
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
        /* The guest is still waiting: its MATCH was lost. */
        if (N.host && N.lobby_done) {
            pc_link_send(N.match_msg, sizeof N.match_msg);
        }
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
        if (len < 7 + HASH) {
            return;
        }
        /* The other side is a break behind: it still needs our reveal. */
        if (msgBreak(m) == N.prev_n && N.prev_revealed) {
            pc_link_send(N.prev_reveal, sizeof N.prev_reveal);
            return;
        }
        /* A break ahead: keep it until we get there. */
        if (msgBreak(m) == N.brk.n + 1) {
            N.early = true;
            N.early_sum = get32(m + 3);
            memcpy(N.early_hash, m + 7, HASH);
            return;
        }
        if (msgBreak(m) != N.brk.n) {
            return;
        }
        /* A repeat means our reveal may have been lost. */
        if (N.brk.got_commit) {
            if (N.brk.sent_reveal) {
                pc_link_send(N.brk.reveal_msg, sizeof N.brk.reveal_msg);
            }
            return;
        }
        N.brk.got_commit = true;
        N.brk.their_sum = get32(m + 3);
        memcpy(N.brk.their_hash, m + 7, HASH);
        return;
    case 'R': {
        u8 check[HASH];

        if (len < 4 + SALT || msgBreak(m) != N.brk.n || !N.brk.got_commit || N.brk.got_reveal) {
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
        setStatus(N.host ? "Waiting for an opponent to join..." : "Joining...");
    } else if (s == PC_LINK_OPEN && !N.hello_sent) {
        setStatus("Opponent here! Pick your fighter and press READY");
    }
    /* Both commits in: the reveal can go. */
    if (N.brk.sent_commit && N.brk.got_commit && !N.brk.sent_reveal) {
        sendReveal();
    }
    /* Anything still unanswered goes again. */
    if (s == PC_LINK_OPEN && ++N.polls % RESEND_POLLS == 0) {
        if (N.hello_sent && !N.lobby_done) {
            pc_link_send(N.hello_msg, sizeof N.hello_msg);
        }
        if (N.brk.sent_commit && !N.brk.got_reveal) {
            pc_link_send(N.brk.commit_msg, sizeof N.brk.commit_msg);
            if (N.brk.sent_reveal) {
                pc_link_send(N.brk.reveal_msg, sizeof N.brk.reveal_msg);
            }
        }
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
        N.hello_msg[0] = 'H';
        N.hello_msg[1] = VERSION;
        N.hello_msg[2] = (u8) my_ckind;
        N.my_ckind = my_ckind;
        pc_link_send(N.hello_msg, sizeof N.hello_msg);
        N.hello_sent = true;
        setStatus(N.host ? "Connected. Waiting for the other player"
                         : "Connected. Waiting for the host");
    }
    tactics_NetPoll();
    /* The host starts the match once both sides are ready, whichever was
     * ready first. */
    if (N.host && N.got_hello && !N.lobby_done) {
        u8* r = N.match_msg;

        if (!pc_link_random(&N.seed, sizeof N.seed)) {
            setStatus("No secure random source for the seed");
            return false;
        }
        r[0] = 'M';
        put32(r + 1, N.seed);
        r[5] = (u8) N.my_ckind;
        r[6] = (u8) N.their_ckind;
        pc_link_send(r, sizeof N.match_msg);
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
    N.prev_n = N.brk.n;
    N.prev_revealed = N.brk.sent_reveal;
    memcpy(N.prev_reveal, N.brk.reveal_msg, sizeof N.prev_reveal);
    memset(&N.brk, 0, sizeof N.brk);
    N.brk.n = n;
    N.brk.my_pick = -1;
    N.brk.their_pick = -1;
    if (N.early && n == N.prev_n + 1) {
        N.brk.got_commit = true;
        N.brk.their_sum = N.early_sum;
        memcpy(N.brk.their_hash, N.early_hash, HASH);
    }
    N.early = false;
}

void tactics_NetSendPick(int pick, u32 sum)
{
    u8* m = N.brk.commit_msg;
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
    pc_link_send(m, sizeof N.brk.commit_msg);
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

bool tactics_NetTheyCommitted(void)
{
    return N.brk.got_commit;
}
