/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The native link (link.h): one TCP connection, messages framed by a 16-bit
 * big-endian length. Non-blocking throughout, serviced by pc_link_state(). */
#include "link.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc.h"
/* extern/dht/dht.h, which wants the socket headers first. */
int dht_random_bytes(void* buf, size_t size);
#include "monocypher.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static bool would_block(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
static void set_nonblocking(sock_t s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static bool would_block(void) {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY;
}
static void set_nonblocking(sock_t s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif

enum { BUF = 16384 };

static struct {
    bool started, host;
    PcLinkState state;
    sock_t listener, conn;
    struct sockaddr_storage peer;
    socklen_t peer_len;
    unsigned char in[BUF], out[BUF];
    int in_len, out_len;
} L = { .listener = SOCK_BAD, .conn = SOCK_BAD };

static void fail(const char* why) {
    if (L.state != PC_LINK_CLOSED) {
        pc_log_line("link: closed (%s)", why);
    }
    if (L.conn != SOCK_BAD) {
        sock_close(L.conn);
        L.conn = SOCK_BAD;
    }
    if (L.listener != SOCK_BAD) {
        sock_close(L.listener);
        L.listener = SOCK_BAD;
    }
    L.state = PC_LINK_CLOSED;
}

static void no_delay(sock_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

static bool start_listen(const char* port) {
    struct sockaddr_in a;
    int on = 1;

    L.listener = socket(AF_INET, SOCK_STREAM, 0);
    if (L.listener == SOCK_BAD) {
        return false;
    }
    setsockopt(L.listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(L.listener, (struct sockaddr*)&a, sizeof a) != 0 || listen(L.listener, 1) != 0) {
        return false;
    }
    set_nonblocking(L.listener);
    pc_log_line("link: hosting on port %s", port);
    return true;
}

static bool resolve_peer(const char* spec) {
    char host[256];
    const char* colon = strrchr(spec, ':');
    struct addrinfo hints, *res = NULL;

    if (colon == NULL || colon - spec >= (int)sizeof host) {
        return false;
    }
    memcpy(host, spec, colon - spec);
    host[colon - spec] = '\0';
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || res == NULL) {
        return false;
    }
    memcpy(&L.peer, res->ai_addr, res->ai_addrlen);
    L.peer_len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    pc_log_line("link: joining %s", spec);
    return true;
}

static void start(void) {
    const char* listen_port = getenv("MELEE_LINK_LISTEN");
    const char* connect_to = getenv("MELEE_LINK_CONNECT");

    L.started = true;
    L.state = PC_LINK_NONE;
    if (listen_port == NULL && connect_to == NULL) {
        return;
    }
#ifdef _WIN32
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif
    L.host = listen_port != NULL;
    L.state = PC_LINK_CONNECTING;
    if (L.host ? !start_listen(listen_port) : !resolve_peer(connect_to)) {
        fail("setup");
    }
}

/* Guest: a non-blocking connect, retried until the host is listening. */
static void try_connect(void) {
    if (L.conn == SOCK_BAD) {
        L.conn = socket(AF_INET, SOCK_STREAM, 0);
        if (L.conn == SOCK_BAD) {
            fail("socket");
            return;
        }
        set_nonblocking(L.conn);
        if (connect(L.conn, (struct sockaddr*)&L.peer, L.peer_len) == 0) {
            L.state = PC_LINK_OPEN;
        } else if (!would_block()) {
            sock_close(L.conn);
            L.conn = SOCK_BAD;
        }
        return;
    }
    {
        fd_set w, e;
        struct timeval tv = { 0, 0 };

        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(L.conn, &w);
        FD_SET(L.conn, &e);
        if (select((int)L.conn + 1, NULL, &w, &e, &tv) <= 0) {
            return;
        }
        if (FD_ISSET(L.conn, &e)) {
            sock_close(L.conn);
            L.conn = SOCK_BAD; /* refused: the host is not up yet */
            return;
        }
        {
            int err = 0;
            socklen_t len = sizeof err;

            getsockopt(L.conn, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err != 0) {
                sock_close(L.conn);
                L.conn = SOCK_BAD;
                return;
            }
        }
        L.state = PC_LINK_OPEN;
    }
}

static void pump(void) {
    int n;

    while (L.out_len > 0) {
        n = send(L.conn, (const char*)L.out, L.out_len, 0);
        if (n <= 0) {
            if (n < 0 && would_block()) {
                break;
            }
            fail("send");
            return;
        }
        memmove(L.out, L.out + n, L.out_len - n);
        L.out_len -= n;
    }
    while (L.in_len < BUF) {
        n = recv(L.conn, (char*)L.in + L.in_len, BUF - L.in_len, 0);
        if (n == 0) {
            fail("the other player left");
            return;
        }
        if (n < 0) {
            if (!would_block()) {
                fail("recv");
            }
            return;
        }
        L.in_len += n;
    }
}

PcLinkState pc_link_state(void) {
    if (!L.started) {
        start();
    }
    if (L.state == PC_LINK_CONNECTING) {
        if (L.host) {
            sock_t c = accept(L.listener, NULL, NULL);

            if (c != SOCK_BAD) {
                sock_close(L.listener);
                L.listener = SOCK_BAD;
                L.conn = c;
                set_nonblocking(c);
                L.state = PC_LINK_OPEN;
            }
        } else {
            try_connect();
        }
        if (L.state == PC_LINK_OPEN) {
            no_delay(L.conn);
            pc_log_line("link: open");
        }
    }
    if (L.state == PC_LINK_OPEN) {
        pump();
    }
    return L.state;
}

bool pc_link_is_host(void) {
    if (!L.started) {
        start();
    }
    return L.host;
}

bool pc_link_send(const void* msg, int len) {
    if (L.state != PC_LINK_OPEN || len <= 0 || len > PC_LINK_MAX_MESSAGE ||
        L.out_len + 2 + len > BUF)
    {
        return false;
    }
    L.out[L.out_len++] = (unsigned char)(len >> 8);
    L.out[L.out_len++] = (unsigned char)len;
    memcpy(L.out + L.out_len, msg, len);
    L.out_len += len;
    pump();
    return L.state == PC_LINK_OPEN;
}

int pc_link_recv(void* buf, int cap) {
    int len;

    if (L.state == PC_LINK_OPEN) {
        pump();
    }
    if (L.in_len < 2) {
        return 0;
    }
    len = (L.in[0] << 8) | L.in[1];
    if (L.in_len < 2 + len) {
        return 0;
    }
    if (len > cap) {
        fail("oversized message");
        return 0;
    }
    memcpy(buf, L.in + 2, len);
    memmove(L.in, L.in + 2 + len, L.in_len - 2 - len);
    L.in_len -= 2 + len;
    return len;
}

void pc_link_close(void) {
    if (L.state == PC_LINK_OPEN || L.state == PC_LINK_CONNECTING) {
        fail("closed here");
    }
}

void pc_link_hash(void* out, int out_len, const void* msg, int len) {
    crypto_blake2b(out, (size_t)out_len, msg, (size_t)len);
}

bool pc_link_random(void* out, int len) {
    return dht_random_bytes(out, (size_t)len) == len;
}
