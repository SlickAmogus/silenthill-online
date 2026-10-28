/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_master.c - Silent Hill Online master server.
 *
 * One file, one UDP socket, one thread. It holds the roster of connected
 * players, fans each map's positions out to the players standing in it, and
 * keeps the marker store that outlives everybody's session.
 *
 * WHY IT IS THIS SMALL. The protocol is stateless per datagram and has no
 * ordering requirements (sh_net_proto.h explains why), so the server needs no
 * connection tracking beyond "when did I last hear from this address", no
 * retransmit buffers and no per-client threads. A select() loop over one
 * socket serves a few hundred players on a home connection, and a home
 * connection is the design target: this is meant to be run by one person for
 * their friends, not operated as a service.
 *
 * BUILD
 *   cmake -S online_server -B online_server/build && cmake --build online_server/build
 * or, without cmake:
 *   Windows:  gcc -O2 -I../pc_port/include sh_master.c -o sh_master.exe -lws2_32
 *   Linux:    cc  -O2 -I../pc_port/include sh_master.c -o sh_master
 *
 * RUN
 *   sh_master -n "Fog" -m "Welcome to Silent Hill"
 *   sh_master -p 27888 -w hunter2 --max 64
 *
 * The listening port must be reachable: forward UDP 27888 to this machine, or
 * keep every player on the same LAN.
 *
 * ADMIN
 *   sh_master --console          (on the same machine, while it runs)
 *   sh_master --cmd "kick Bob"
 * players, kick, ban, prune old markers and more; `help` lists them.
 *
 * BROWSER PLAYERS
 *   sh_master --ws 27890
 * also takes WebSocket clients on TCP 27890. The browser build needs wss://,
 * so put a TLS reverse proxy in front of it (Caddy:
 * `server.example.com { reverse_proxy 127.0.0.1:27890 }`).
 */

/* rand_s is only declared when this is set before the first stdlib.h, and
 * windows.h pulls stdlib.h in, so it has to precede every include. */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#if defined(_WIN32)
#if !defined(SH_MASTER_EMBED) && !defined(FD_SETSIZE)
/* Winsock's default of 64 is fewer sockets than the WebSocket clients alone. */
#define FD_SETSIZE 1024
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET    ShSock;
typedef int       ShSockLen;
#define SH_INVALID_SOCK INVALID_SOCKET
#define SH_CLOSESOCK    closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
/* Android ships the header from API 21 but getrandom() only from API 28; the
 * embedded relay in the app targets 21, so it reads /dev/urandom there. */
#if defined(__linux__) && defined(__has_include) && (!defined(__ANDROID__) || __ANDROID_API__ >= 28)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define SH_HAVE_GETRANDOM 1
#endif
#endif
typedef int       ShSock;
typedef socklen_t ShSockLen;
#define SH_INVALID_SOCK (-1)
#define SH_CLOSESOCK    close
#endif

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_net_proto.h"

#ifdef SH_MASTER_EMBED
#include "sh_master_embed.h"
#include "sh_log.h" /* embedded in the client: relay logs go to SilentHill.log */
#endif

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define SRV_MAX_CLIENTS   1024
#define SRV_MAX_MEMOS     20000
#define SRV_MEMOS_PER_MAP 400
#define SRV_MEMOS_PER_USER 40
#define SRV_MAP_COUNT     64   /* e_MapIdx tops out at 42; 64 covers it */

#define SRV_TICK_MS       100  /* snapshot rate; matches the client's STATE rate */
#define SRV_TIMEOUT_MS    15000
#define SRV_SAVE_GAP_MS   30000
/* A client that floods gets its extra datagrams dropped rather than getting
 * disconnected: a burst is far more likely to be a bad network than an
 * attacker, and dropping is free. */
/* Co-op peers stream poses every frame (60/s, one room-wide relay each) on
 * top of STATE, pings, items and hits. */
#define SRV_RATE_PER_SEC  200

/* Two death markers within this distance and this long apart are one death.
 * Q19.12 metres: 1.5m, 30s. */
#define SRV_DEATH_DEDUPE_DIST (((1 << 12) * 3) / 2)
#define SRV_DEATH_DEDUPE_MS   30000

/* A HELLO cookie is valid for its 30 s bucket and the one before, so a client
 * that got its CHALLENGE just before a bucket edge still gets in. */
#define SRV_COOKIE_BUCKET_MS  30000

/* REJECTs go to an address that has not proven it receives there; a global
 * cap keeps the server useless as a reflector however many sources are forged. */
#define SRV_REJECTS_PER_SEC   50

/* Online password guessing: a few typos per minute, then silence. */
#define SRV_PASS_FAILS_PER_MIN 5
#define SRV_PASS_TABLE        64

#define SRV_WELCOME_GAP_MS    500
#define SRV_MAX_PER_IP_DEFAULT 8

/* Datagrams handled per select() pass: enough for a full server's normal
 * traffic, few enough that a junk flood cannot starve the timers below it. */
#define SRV_RX_PER_PASS       256

#define SRV_MEMO_GAP_MS       5000
#define SRV_RATING_CLAMP      1000
#define SRV_VOTE_BUCKETS      1024
#define SRV_VOTE_WAYS         4

#define SRV_JOIN_FAILS_MAX    10
#define SRV_JOIN_BLOCK_MS     60000

#define SRV_CHARA_MAX         64

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int                used;
    shn_u32            playerId;
    shn_u32            uid;      /* the client's persistent id; 0 from an older client */
    shn_u32            session;
    struct sockaddr_in addr;

    char               name[SHNET_NAME_MAX];
    int                mapIdx;
    int                charaId;
    int                flags;
    shn_s32            x, y, z;
    shn_s16            rotY;
    shn_s16            health;
    shn_u16            anim, frame;

    unsigned int       lastRxMs;
    unsigned int       joinMs;
    unsigned int       lastSnapMs;
    int                pingMs;

    /* Rate limiter: a packet budget refilled once a second. */
    unsigned int       rateWindowMs;
    int                rateCount;

    unsigned int       lastDeathMs;
    shn_s32            lastDeathX, lastDeathZ;
    int                memoCount;
    int                loggedSnap;

    unsigned int       lastWelcomeMs;
    unsigned int       lastMemoMs;
    int                memoPlaced;   /* lastMemoMs is meaningful */
    int                joinFails;
    unsigned int       joinBlockUntilMs;

    /* Room host: addresses it removed, refused if they try to come back. */
    struct in_addr     kickedIp[8];
    int                kickedN;
    int                joinBlocked;

    /* Co-op room. room == 0 means not in one. The host is the client that
     * created it (roomHost); roomMax is the player cap, kept on every member. */
    shn_u16            room;
    int                roomHost;
    int                roomMax;
    int                roomHidden; /* host chose not to list it; join by code only */

    /* The client's build id from HELLO. Co-op peers must run the same build:
     * one push can change what a packet means without touching the protocol
     * version, so a room only admits its host's build. */
    char               build[SHNET_BUILD_MAX];
} Client;

typedef struct
{
    shn_u32 memoId;
    shn_u32 ownerId;
    char    ownerName[SHNET_NAME_MAX];
    int     kind;
    int     mapIdx;
    shn_s32 x, y, z;
    shn_s16 rotY;
    shn_s16 rating;
    shn_u16 phraseA, wordA, phraseB, wordB;
    long    placedUnix;
    /* Placer's IPv4 (network order), memory only: the quota is per address
     * because a playerId is fresh on every HELLO. Never sent, never saved, so
     * markers loaded from disk carry 0 and count against nobody. */
    shn_u32 ownerIp;
} Memo;

static Client        g_clients[SRV_MAX_CLIENTS];
static Memo*         g_memos;
static int           g_memoCount;
static shn_u32       g_nextMemoId = 1;
static shn_u32       g_nextPlayerId = 1;
/* Who a client IS across visits: its persistent id, or for an older client
 * the session's player id (which is what every memo was attributed to
 * before, and why a newcomer could be handed someone else's). */
#define SRV_IDENT(c) ((c)->uid ? (c)->uid : (c)->playerId)
static shn_u8        g_cookieKey[16];

static ShSock        g_sock = SH_INVALID_SOCK;
static unsigned short g_port = SHNET_DEFAULT_PORT;
static char          g_serverName[SHNET_SERVER_MAX] = "Silent Hill";
static char          g_motd[SHNET_MOTD_MAX]         = "";
static char          g_password[256];
static int           g_maxClients = 512;
static int           g_maxPerIp   = SRV_MAX_PER_IP_DEFAULT;

static unsigned int  g_rejectWindowMs;
static int           g_rejectCount;

typedef struct
{
    shn_u32      ip;
    unsigned int windowMs;
    int          fails;
} PassFail;
static PassFail      g_passFails[SRV_PASS_TABLE];

typedef struct
{
    shn_u32 ip;
    shn_u32 memoId;
} Vote;
static Vote          g_votes[SRV_VOTE_BUCKETS][SRV_VOTE_WAYS];
static unsigned char g_voteNext[SRV_VOTE_BUCKETS];
static int           g_featureFlags = SHNET_SF_GHOSTS | SHNET_SF_MEMOS |
                                      SHNET_SF_DEATHS | SHNET_SF_EVENTS;
static char          g_dbPath[512] = "memos.db";
static int           g_dbDirty;
static unsigned int  g_lastSaveMs;
static volatile int  g_running = 1;
static int           g_verbose;

/* Build allow-list (-b ID, --builds FILE). Empty = any build may connect
 * (the default). Entries are the short id the client sends in HELLO (the first
 * SHNET_BUILD_MAX-1 hex chars of its executable's SHA-256); a full 64-char
 * hash pasted by the admin is truncated to match. */
#define SRV_BUILDS_MAX 32
static char g_allowedBuilds[SRV_BUILDS_MAX][SHNET_BUILD_MAX];
static int  g_allowedBuildCount;

#ifndef SH_MASTER_EMBED /* the allow-list is set from the command line only */
static void SrvAllowBuild(const char* id)
{
    char tmp[SHNET_BUILD_MAX];
    int  n = 0;
    while (*id == ' ' || *id == '\t') id++;
    while (*id && *id != ' ' && *id != '\t' && *id != '\r' && *id != '\n' && n < SHNET_BUILD_MAX - 1)
    {
        tmp[n++] = *id++;
    }
    tmp[n] = '\0';
    if (n == 0 || tmp[0] == '#' || g_allowedBuildCount >= SRV_BUILDS_MAX) return;
    memcpy(g_allowedBuilds[g_allowedBuildCount++], tmp, sizeof(tmp));
}

/* One id per line; blank lines and '#' comments ignored. */
static void SrvLoadBuildsFile(const char* path)
{
    FILE* f = fopen(path, "r");
    char  line[256];
    if (!f)
    {
        fprintf(stderr, "could not read build allow-list %s\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f))
    {
        SrvAllowBuild(line);
    }
    fclose(f);
}
#endif

/* ------------------------------------------------------------------ */
/* Platform odds and ends                                              */
/* ------------------------------------------------------------------ */

static unsigned int SrvMillis(void)
{
#if defined(_WIN32)
    return (unsigned int)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

#ifndef SH_MASTER_EMBED
static void SrvOnSignal(int sig)
{
    (void)sig;
    g_running = 0;
}
#endif

static void SrvLog(const char* fmt, ...)
{
    va_list ap;
#ifdef SH_MASTER_EMBED
    char line[256];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SH_DBG("[coop-relay] %s", line);
#else
    time_t    t  = time(NULL);
    struct tm* lt = localtime(&t);
    char      stamp[32];

    strftime(stamp, sizeof(stamp), "%H:%M:%S", lt);
    printf("[%s] ", stamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
#endif
}

static int SrvOsRandom(shn_u8* buf, int n)
{
#if defined(_WIN32)
    int i = 0;
    while (i < n)
    {
        unsigned int v;
        int          k;
        if (rand_s(&v) != 0)
        {
            return 0;
        }
        for (k = 0; k < 4 && i < n; k++, i++)
        {
            buf[i] = (shn_u8)(v >> (8 * k));
        }
    }
    return 1;
#else
    int   got = 0;
    FILE* f;
#ifdef SH_HAVE_GETRANDOM
    while (got < n)
    {
        ssize_t r = getrandom(buf + got, (size_t)(n - got), 0);
        if (r <= 0)
        {
            break;
        }
        got += (int)r;
    }
    if (got == n)
    {
        return 1;
    }
    got = 0;
#endif
    f = fopen("/dev/urandom", "rb");
    if (!f)
    {
        return 0;
    }
    got = (int)fread(buf, 1, (size_t)n, f);
    fclose(f);
    return got == n;
#endif
}

/* Session ids, room codes and the cookie key must not be predictable from
 * anything a client can observe, which rules out any PRNG whose output is its
 * state. The xorshift fallback only exists so a box with no usable OS source
 * still runs; it is said loudly because it reopens session guessing. */
static void SrvRandomBytes(void* out, int n)
{
    static int     warned;
    static shn_u32 fb;
    shn_u8*        buf = (shn_u8*)out;
    int            i;

    if (SrvOsRandom(buf, n))
    {
        return;
    }
    if (!warned)
    {
        warned = 1;
        SrvLog("WARNING: no OS random source; session ids and cookies are guessable");
    }
    if (fb == 0)
    {
#if defined(_WIN32)
        fb = (shn_u32)time(NULL) ^ ((shn_u32)GetCurrentProcessId() << 16) ^ SrvMillis();
#else
        fb = (shn_u32)time(NULL) ^ ((shn_u32)getpid() << 16) ^ SrvMillis();
#endif
        fb ^= (shn_u32)(size_t)&warned * 2654435761u;
        if (fb == 0) fb = 0x9E3779B9u;
    }
    for (i = 0; i < n; i++)
    {
        fb ^= fb << 13;
        fb ^= fb >> 17;
        fb ^= fb << 5;
        buf[i] = (shn_u8)(fb >> 24);
    }
}

/* ------------------------------------------------------------------ */
/* Client table                                                        */
/* ------------------------------------------------------------------ */

/* The first three bytes of sin_zero mark a WebSocket client and its slot
 * (see the WebSocket section); a UDP address has them zeroed. */
static int SrvAddrEqual(const struct sockaddr_in* a, const struct sockaddr_in* b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port &&
           memcmp(a->sin_zero, b->sin_zero, 3) == 0;
}

static void SrvAddrStr(const struct sockaddr_in* a, char* out, int cap)
{
    unsigned int ip = ntohl(a->sin_addr.s_addr);
    if (a->sin_zero[0])
    {
        snprintf(out, (size_t)cap, "%u.%u.%u.%u (web %u)",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                 (unsigned)(a->sin_zero[1] | (a->sin_zero[2] << 8)));
        return;
    }
    snprintf(out, (size_t)cap, "%u.%u.%u.%u:%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
             (unsigned)ntohs(a->sin_port));
}

static Client* SrvFindByAddr(const struct sockaddr_in* a)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && SrvAddrEqual(&g_clients[i].addr, a))
        {
            return &g_clients[i];
        }
    }
    return NULL;
}

static Client* SrvFindBySession(shn_u32 session)
{
    int i;
    if (session == 0)
    {
        return NULL;
    }
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && g_clients[i].session == session)
        {
            return &g_clients[i];
        }
    }
    return NULL;
}

/* Session ids authorise the reconnect path, which rebinds a live session to a
 * new address, so one player's id must say nothing about anyone else's. */
static shn_u32 SrvNewSession(void)
{
    shn_u32 v;
    do
    {
        SrvRandomBytes(&v, (int)sizeof(v));
    } while (v == 0 || SrvFindBySession(v) != NULL);
    return v;
}

static int SrvClientsFromIp(shn_u32 ip)
{
    int i, n = 0;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && g_clients[i].addr.sin_addr.s_addr == ip) n++;
    }
    return n;
}

static shn_u32 SrvCookie(const struct sockaddr_in* a, unsigned int bucket)
{
    shn_u8  d[10];
    shn_u32 ip = a->sin_addr.s_addr;
    shn_u16 pt = a->sin_port;
    shn_u32 v;

    memcpy(d, &ip, 4);
    memcpy(d + 4, &pt, 2);
    d[6] = (shn_u8)bucket;
    d[7] = (shn_u8)(bucket >> 8);
    d[8] = (shn_u8)(bucket >> 16);
    d[9] = (shn_u8)(bucket >> 24);
    v = (shn_u32)ShnSipHash(g_cookieKey, d, (int)sizeof(d));
    return v ? v : 1u; /* 0 is what a client sends when it has no cookie yet */
}

static int SrvCookieValid(const struct sockaddr_in* a, shn_u32 cookie)
{
    unsigned int bucket = SrvMillis() / SRV_COOKIE_BUCKET_MS;
    if (cookie == 0)
    {
        return 0;
    }
    return cookie == SrvCookie(a, bucket) || cookie == SrvCookie(a, bucket - 1u);
}

/* Returns 1 while this address may still try a password. */
static int SrvPassAllowed(shn_u32 ip, unsigned int now)
{
    int i;
    for (i = 0; i < SRV_PASS_TABLE; i++)
    {
        PassFail* f = &g_passFails[i];
        if (f->fails && f->ip == ip && (now - f->windowMs) < 60000u)
        {
            return f->fails < SRV_PASS_FAILS_PER_MIN;
        }
    }
    return 1;
}

static void SrvPassFailed(shn_u32 ip, unsigned int now)
{
    int i, oldest = -1;
    for (i = 0; i < SRV_PASS_TABLE; i++)
    {
        PassFail* f = &g_passFails[i];
        if (f->fails && f->ip == ip)
        {
            if ((now - f->windowMs) >= 60000u)
            {
                f->windowMs = now;
                f->fails    = 0;
            }
            f->fails++;
            return;
        }
    }
    for (i = 0; i < SRV_PASS_TABLE; i++)
    {
        if (!g_passFails[i].fails)
        {
            oldest = i;
            break;
        }
        if (oldest < 0 || (now - g_passFails[i].windowMs) > (now - g_passFails[oldest].windowMs))
        {
            oldest = i;
        }
    }
    g_passFails[oldest].ip       = ip;
    g_passFails[oldest].windowMs = now;
    g_passFails[oldest].fails    = 1;
}

static int SrvClientCount(void)
{
    int i, n = 0;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used) n++;
    }
    return n;
}

static void SrvSanitize(char* s, int cap)
{
    int i;
    int keep = 0;
    for (i = 0; i < cap - 1 && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126)
        {
            s[i] = '?';
        }
        if (s[i] != ' ')
        {
            keep = 1;
        }
    }
    s[cap - 1] = '\0';
    if (!keep)
    {
        snprintf(s, (size_t)cap, "Wanderer");
    }
}

/* Chat must not share SrvSanitize's default: a blank line has to stay blank so
 * the caller drops it, instead of broadcasting the word "Wanderer". */
static void SrvSanitizeChat(char* s, int cap)
{
    int i;
    int keep = 0;
    for (i = 0; i < cap - 1 && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126)
        {
            s[i] = '?';
        }
        if (s[i] != ' ')
        {
            keep = 1;
        }
    }
    s[cap - 1] = '\0';
    if (!keep)
    {
        s[0] = '\0';
    }
}

static shn_s32 SrvClampCoord(shn_s32 v)
{
    if (v > SHNET_COORD_MAX)  return SHNET_COORD_MAX;
    if (v < -SHNET_COORD_MAX) return -SHNET_COORD_MAX;
    return v;
}

static int SrvCoordOk(shn_s32 v)
{
    return v >= -SHNET_COORD_MAX && v <= SHNET_COORD_MAX;
}

/* ------------------------------------------------------------------ */
/* Send helpers                                                        */
/* ------------------------------------------------------------------ */

#ifndef SH_MASTER_EMBED
static void WsSend(const struct sockaddr_in* to, const shn_u8* buf, int len);
#endif

static void SrvSend(const struct sockaddr_in* to, const shn_u8* buf, int len)
{
#ifndef SH_MASTER_EMBED
    if (to->sin_zero[0])
    {
        WsSend(to, buf, len);
        return;
    }
#endif
    sendto(g_sock, (const char*)buf, (size_t)len, 0,
           (const struct sockaddr*)to, (ShSockLen)sizeof(*to));
}

static void SrvSendTo(Client* c, shn_u8* buf, int payloadLen, int type)
{
    ShnPutHeader(buf, (shn_u8)type, (shn_u16)payloadLen, c->session);
    SrvSend(&c->addr, buf, SHNET_HDR_SIZE + payloadLen);
}

static void SrvSendEvent(Client* c, int kind, shn_u32 who, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8 + SHNET_EVENT_MAX];
    int    off = SHNET_HDR_SIZE;

    if (!(g_featureFlags & SHNET_SF_EVENTS))
    {
        return;
    }
    ShnPutU8(buf, &off, (shn_u8)kind);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, who);
    ShnPutStr(buf, &off, text, SHNET_EVENT_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_EVENT);
}

static void SrvBroadcastEvent(int kind, shn_u32 who, const char* text, Client* except)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && &g_clients[i] != except)
        {
            SrvSendEvent(&g_clients[i], kind, who, text);
        }
    }
}

static void SrvSendChat(Client* c, int scope, shn_u32 fromId, const char* name, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8 + SHNET_NAME_MAX + SHNET_CHAT_MAX];
    int    off = SHNET_HDR_SIZE;

    ShnPutU8(buf, &off, (shn_u8)scope);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutU32(buf, &off, fromId);
    ShnPutStr(buf, &off, name, SHNET_NAME_MAX);
    ShnPutStr(buf, &off, text, SHNET_CHAT_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_CHAT_MSG);
}

static void SrvHandleChatSay(Client* c, const shn_u8* p, int len)
{
    int  off = 0;
    int  scope;
    char text[SHNET_CHAT_MAX];
    int  i;

    if (len < 4 + SHNET_CHAT_MAX)
    {
        return;
    }
    scope = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    (void)ShnGetU16(p, &off);
    ShnGetStr(p, &off, SHNET_CHAT_MAX, text, sizeof(text));
    SrvSanitizeChat(text, SHNET_CHAT_MAX);
    if (text[0] == '\0')
    {
        return;
    }
    if (scope != SHNET_CHAT_GLOBAL && scope != SHNET_CHAT_GAME)
    {
        scope = SHNET_CHAT_GLOBAL;
    }

    SrvLog("  chat[%s] %s: %s", scope == SHNET_CHAT_GAME ? "game" : "all", c->name, text);

    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* o = &g_clients[i];
        if (!o->used)
        {
            continue;
        }
        /* Game chat only reaches players standing in the same map. A client
         * with no map yet (still on a menu) is not in anyone's game. */
        if (scope == SHNET_CHAT_GAME && (o->mapIdx < 0 || o->mapIdx != c->mapIdx))
        {
            continue;
        }
        SrvSendChat(o, scope, c->playerId, c->name, text);
    }
}

static void SrvSendReject(const struct sockaddr_in* to, int reason, const char* text)
{
    shn_u8       buf[SHNET_HDR_SIZE + 4 + SHNET_REJECT_MAX];
    int          off = SHNET_HDR_SIZE;
    unsigned int now = SrvMillis();

    if (now - g_rejectWindowMs >= 1000u)
    {
        g_rejectWindowMs = now;
        g_rejectCount    = 0;
    }
    if (++g_rejectCount > SRV_REJECTS_PER_SEC)
    {
        return;
    }
    ShnPutU8(buf, &off, (shn_u8)reason);
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutStr(buf, &off, text, SHNET_REJECT_MAX);
    ShnPutHeader(buf, SHNET_MSG_REJECT, (shn_u16)(off - SHNET_HDR_SIZE), 0);
    SrvSend(to, buf, off);
}

static void SrvSendChallenge(const struct sockaddr_in* to)
{
    shn_u8 buf[SHNET_HDR_SIZE + 4];
    int    off = SHNET_HDR_SIZE;

    ShnPutU32(buf, &off, SrvCookie(to, SrvMillis() / SRV_COOKIE_BUCKET_MS));
    ShnPutHeader(buf, SHNET_MSG_CHALLENGE, 4, 0);
    SrvSend(to, buf, off);
}

static void SrvSendWelcome(Client* c)
{
    shn_u8 buf[SHNET_HDR_SIZE + 16 + SHNET_SERVER_MAX + SHNET_MOTD_MAX];
    int    off = SHNET_HDR_SIZE;

    ShnPutU32(buf, &off, c->session);
    ShnPutU32(buf, &off, c->playerId);
    ShnPutU16(buf, &off, SHNET_PROTO_VER);
    ShnPutU16(buf, &off, SRV_TICK_MS);
    ShnPutU16(buf, &off, SHNET_SNAP_MAX);
    ShnPutU16(buf, &off, (shn_u16)g_featureFlags);
    ShnPutStr(buf, &off, g_serverName, SHNET_SERVER_MAX);
    ShnPutStr(buf, &off, g_motd, SHNET_MOTD_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_WELCOME);
}

/* ------------------------------------------------------------------ */
/* Marker store                                                        */
/* ------------------------------------------------------------------ */

static int SrvMemosOnMap(int mapIdx)
{
    int i, n = 0;
    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].mapIdx == mapIdx) n++;
    }
    return n;
}

/* Drop the lowest-rated, oldest marker on a full map so a busy area keeps
 * turning over instead of freezing at whatever was placed first. */
static void SrvEvictOnMap(int mapIdx)
{
    int i, worst = -1;
    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].mapIdx != mapIdx)
        {
            continue;
        }
        if (worst < 0 ||
            g_memos[i].rating < g_memos[worst].rating ||
            (g_memos[i].rating == g_memos[worst].rating &&
             g_memos[i].placedUnix < g_memos[worst].placedUnix))
        {
            worst = i;
        }
    }
    if (worst >= 0)
    {
        g_memos[worst] = g_memos[--g_memoCount];
        g_dbDirty = 1;
    }
}

static void SrvSaveDb(void)
{
    FILE* f;
    int   i;

    if (!g_dbPath[0])
    {
        return;
    }
    f = fopen(g_dbPath, "w");
    if (!f)
    {
        SrvLog("could not write %s", g_dbPath);
        return;
    }
    /* Plain text on purpose: an admin has to be able to delete one bad marker
     * with a text editor without a tool for it. */
    fprintf(f, "# sh_master marker store v1\n");
    fprintf(f, "# id owner kind map x y z rot rating phraseA wordA phraseB wordB unix name\n");
    for (i = 0; i < g_memoCount; i++)
    {
        const Memo* m = &g_memos[i];
        fprintf(f, "%u %u %d %d %d %d %d %d %d %u %u %u %u %ld %s\n",
                m->memoId, m->ownerId, m->kind, m->mapIdx,
                (int)m->x, (int)m->y, (int)m->z, (int)m->rotY, (int)m->rating,
                (unsigned)m->phraseA, (unsigned)m->wordA,
                (unsigned)m->phraseB, (unsigned)m->wordB,
                m->placedUnix, m->ownerName);
    }
    fclose(f);
    g_dbDirty = 0;
}

static void SrvLoadDb(void)
{
    FILE* f = fopen(g_dbPath, "r");
    char  line[512];

    if (!f)
    {
        SrvLog("no marker store at %s (starting empty)", g_dbPath);
        return;
    }
    while (fgets(line, sizeof(line), f))
    {
        Memo m;
        unsigned int id, owner, pa, wa, pb, wb;
        int kind, map, x, y, z, rot, rating;
        long unixT;
        char nm[64];

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }
        nm[0] = '\0';
        if (sscanf(line, "%u %u %d %d %d %d %d %d %d %u %u %u %u %ld %63[^\n]",
                   &id, &owner, &kind, &map, &x, &y, &z, &rot, &rating,
                   &pa, &wa, &pb, &wb, &unixT, nm) < 14)
        {
            continue;
        }
        if (g_memoCount >= SRV_MAX_MEMOS)
        {
            break;
        }
        /* A store written before the server checked template indices may
         * still hold bad ones; they must not be served again. */
        if (map < 0 || map >= SRV_MAP_COUNT ||
            !SrvCoordOk(x) || !SrvCoordOk(y) || !SrvCoordOk(z) ||
            ((kind & 0x0F) == SHNET_MARK_MEMO &&
             (pa >= SHNET_MEMO_PHRASE_MAX || wa >= SHNET_MEMO_WORD_MAX ||
              (pb != SHNET_MEMO_NONE && pb >= SHNET_MEMO_PHRASE_MAX) ||
              (wb != SHNET_MEMO_NONE && wb >= SHNET_MEMO_WORD_MAX))))
        {
            continue;
        }
        if (rating > SRV_RATING_CLAMP)  rating = SRV_RATING_CLAMP;
        if (rating < -SRV_RATING_CLAMP) rating = -SRV_RATING_CLAMP;
        memset(&m, 0, sizeof(m));
        m.memoId  = id;
        m.ownerId = owner;
        m.kind    = kind;
        m.mapIdx  = map;
        m.x       = x;
        m.y       = y;
        m.z       = z;
        m.rotY    = (shn_s16)rot;
        m.rating  = (shn_s16)rating;
        m.phraseA = (shn_u16)pa;
        m.wordA   = (shn_u16)wa;
        m.phraseB = (shn_u16)pb;
        m.wordB   = (shn_u16)wb;
        m.placedUnix = unixT;
        snprintf(m.ownerName, sizeof(m.ownerName), "%s", nm[0] ? nm : "?");
        SrvSanitize(m.ownerName, SHNET_NAME_MAX);

        g_memos[g_memoCount++] = m;
        if (id >= g_nextMemoId)
        {
            g_nextMemoId = id + 1;
        }
    }
    fclose(f);
    SrvLog("loaded %d markers from %s (next id %u)", g_memoCount, g_dbPath, g_nextMemoId);
}

/* ------------------------------------------------------------------ */
/* Message handlers                                                    */
/* ------------------------------------------------------------------ */

#ifndef SH_MASTER_EMBED
static int AdmIsBanned(shn_u32 ipNet, shn_u32 uid);
#endif

static void SrvHandleHello(const struct sockaddr_in* from, const shn_u8* p, int len)
{
    int      off = 0;
    shn_u16  ver;
    int      charaId;
    shn_u32  reconnect;
    char     name[SHNET_NAME_MAX];
    char     build[SHNET_BUILD_MAX];
    shn_u32  proof;
    shn_u32  cookie;
    shn_u32  uid;
    Client*  c;
    char     addr[64];
    char     text[SHNET_EVENT_MAX];
    unsigned int now = SrvMillis();
    int      fresh = 0;

    /* Shorter than even the old layout is not a HELLO; answering it at all
     * would turn a 12-byte datagram into a 112-byte one. */
    if (len < SHNET_HELLO_LEN_OLD)
    {
        return;
    }
    if (len < SHNET_HELLO_LEN)
    {
        SrvSendReject(from, SHNET_REJ_VERSION, "Update your game to play online");
        return;
    }
    ver       = ShnGetU16(p, &off);
    (void)ShnGetU8(p, &off);
    charaId   = (int)ShnGetU8(p, &off);
    reconnect = ShnGetU32(p, &off);
    ShnGetStr(p, &off, SHNET_NAME_MAX, name, SHNET_NAME_MAX);
    ShnGetStr(p, &off, SHNET_BUILD_MAX, build, SHNET_BUILD_MAX);
    proof  = ShnGetU32(p, &off);
    cookie = ShnGetU32(p, &off);
    uid    = (len >= SHNET_HELLO_LEN_UID) ? ShnGetU32(p, &off) : 0;

    if (ver != SHNET_PROTO_VER)
    {
        SrvSendReject(from, SHNET_REJ_VERSION, "Wrong protocol version");
        return;
    }
    /* Nothing below runs, logs or allocates for a sender that has not shown it
     * receives at its source address; forged sources stop here. */
    if (!SrvCookieValid(from, cookie))
    {
        SrvSendChallenge(from);
        return;
    }

    SrvAddrStr(from, addr, (int)sizeof(addr));

#ifndef SH_MASTER_EMBED
    if (AdmIsBanned(from->sin_addr.s_addr, uid))
    {
        SrvSendReject(from, SHNET_REJ_BANNED, "You are banned from this server");
        if (g_verbose) SrvLog("rejected %s: banned", addr);
        return;
    }
#endif

    if (g_password[0] && proof != ShnPasswordProof(g_password, cookie))
    {
        if (!SrvPassAllowed(from->sin_addr.s_addr, now))
        {
            return;
        }
        SrvPassFailed(from->sin_addr.s_addr, now);
        SrvSendReject(from, SHNET_REJ_PASSWORD, "Password required or incorrect");
        SrvLog("rejected %s: bad password", addr);
        return;
    }
    if (charaId >= SRV_CHARA_MAX)
    {
        charaId = 0;
    }
    /* Build allow-list: a deterrent against casually modified clients (the id
     * is the client exe's hash). Off unless the admin listed at least one id. */
    if (g_allowedBuildCount > 0)
    {
        int ok = 0, k;
        for (k = 0; k < g_allowedBuildCount; k++)
        {
            if (strncmp(build, g_allowedBuilds[k], SHNET_BUILD_MAX - 1) == 0) { ok = 1; break; }
        }
        if (!ok)
        {
            SrvSendReject(from, SHNET_REJ_BUILD, "This game build is not approved on this server");
            SrvLog("rejected %s: build %s is not on the allow-list", addr, build);
            return;
        }
    }

    SrvSanitize(name, SHNET_NAME_MAX);

    /* An identical HELLO from an address already in the table is a retry the
     * WELCOME did not reach: answer again with the SAME session, or the client
     * would get a new identity every second of packet loss. */
    c = SrvFindByAddr(from);
    if (!c && reconnect)
    {
        /* A NAT rebinding changes the port, not the address. Requiring the
         * same IP means a leaked session id is useless from anywhere else. */
        c = SrvFindBySession(reconnect);
        if (c && c->addr.sin_addr.s_addr != from->sin_addr.s_addr)
        {
            c = NULL;
        }
    }

    if (!c)
    {
        int i;
        if (SrvClientCount() >= g_maxClients)
        {
            SrvSendReject(from, SHNET_REJ_FULL, "Server is full");
            return;
        }
        if (SrvClientsFromIp(from->sin_addr.s_addr) >= g_maxPerIp)
        {
            SrvSendReject(from, SHNET_REJ_FULL, "Too many players from your address");
            SrvLog("rejected %s: %d players already on that address", addr, g_maxPerIp);
            return;
        }
        for (i = 0; i < SRV_MAX_CLIENTS; i++)
        {
            if (!g_clients[i].used)
            {
                c = &g_clients[i];
                break;
            }
        }
        if (!c)
        {
            SrvSendReject(from, SHNET_REJ_FULL, "Server is full");
            return;
        }
        memset(c, 0, sizeof(*c));
        c->used     = 1;
        c->playerId = g_nextPlayerId++;
        c->session  = SrvNewSession();
        c->joinMs   = SrvMillis();
        c->mapIdx   = -1;

        fresh       = 1;

        snprintf(text, sizeof(text), "%s entered the fog", name);
        SrvBroadcastEvent(SHNET_EV_JOIN, c->playerId, text, c);
        SrvLog("+ %s joined from %s (id %u, build %s)", name, addr, c->playerId, build);
    }

    c->addr    = *from;
    c->charaId = charaId;
    c->lastRxMs = now;
    memcpy(c->name, name, SHNET_NAME_MAX);
    memcpy(c->build, build, SHNET_BUILD_MAX);
    c->uid = uid;

    /* A client retries HELLO until it hears WELCOME; one answer per gap is
     * enough to get through loss and keeps a HELLO loop from being a pump. */
    if (!fresh && (now - c->lastWelcomeMs) < SRV_WELCOME_GAP_MS)
    {
        return;
    }
    c->lastWelcomeMs = now;
    SrvSendWelcome(c);
    if (fresh && g_motd[0])
    {
        SrvSendEvent(c, SHNET_EV_SERVER, 0, g_motd);
    }
}

static void SrvHandleState(Client* c, const shn_u8* p, int len)
{
    int off = 0;
    if (len < 28)
    {
        return;
    }
    (void)ShnGetU32(p, &off); /* sequence; ordering does not matter for ghosts */
    c->mapIdx  = (int)ShnGetU8(p, &off);
    c->charaId = (int)ShnGetU8(p, &off);
    c->flags   = (int)ShnGetU8(p, &off);
    (void)ShnGetU8(p, &off);
    c->x      = ShnGetS32(p, &off);
    c->y      = ShnGetS32(p, &off);
    c->z      = ShnGetS32(p, &off);
    c->rotY   = ShnGetS16(p, &off);
    c->health = ShnGetS16(p, &off);
    c->anim   = ShnGetU16(p, &off);
    c->frame  = ShnGetU16(p, &off);

    if (c->mapIdx < 0 || c->mapIdx >= SRV_MAP_COUNT)
    {
        c->mapIdx = -1;
    }
    if (c->charaId >= SRV_CHARA_MAX)
    {
        c->charaId = 0;
    }
    c->x = SrvClampCoord(c->x);
    c->y = SrvClampCoord(c->y);
    c->z = SrvClampCoord(c->z);
}

static void SrvHandlePing(Client* c, const shn_u8* p, int len)
{
    shn_u8 buf[SHNET_HDR_SIZE + 8];
    int    off = SHNET_HDR_SIZE;
    int    ro  = 0;
    shn_u32 clientMs;

    if (len < 4)
    {
        return;
    }
    clientMs = ShnGetU32(p, &ro);
    ShnPutU32(buf, &off, clientMs);
    ShnPutU32(buf, &off, SrvMillis());
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_PONG);
}

static void SrvHandleRosterReq(Client* c)
{
    shn_u8 buf[SHNET_MTU];
    int    i;
    int    sent  = 0;
    int    total = SrvClientCount();
    unsigned int now = SrvMillis();

    i = 0;
    while (sent < total)
    {
        int off   = SHNET_HDR_SIZE;
        int count = 0;
        int hdrAt;

        hdrAt = off;
        ShnPutU16(buf, &off, (shn_u16)total);
        ShnPutU16(buf, &off, 0); /* count, back-patched */
        ShnPutU16(buf, &off, (shn_u16)sent);
        ShnPutU16(buf, &off, 0);

        for (; i < SRV_MAX_CLIENTS && count < SHNET_ROSTER_MAX; i++)
        {
            Client* o = &g_clients[i];
            if (!o->used)
            {
                continue;
            }
            ShnPutU32(buf, &off, o->playerId);
            ShnPutU8(buf, &off, (shn_u8)(o->mapIdx < 0 ? 0xFF : o->mapIdx));
            ShnPutU8(buf, &off, (shn_u8)o->charaId);
            ShnPutU8(buf, &off, (shn_u8)o->flags);
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, (shn_u16)(o->pingMs > 65535 ? 65535 : o->pingMs));
            ShnPutU16(buf, &off, (shn_u16)((now - o->joinMs) / 60000u));
            ShnPutStr(buf, &off, o->name, SHNET_NAME_MAX);
            count++;
        }

        {
            int patch = hdrAt + 2;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_ROSTER);

        sent += count;
        if (count == 0)
        {
            break;
        }
    }
}

static void SrvHandleMemoQuery(Client* c, const shn_u8* p, int len)
{
    shn_u8 buf[SHNET_MTU];
    int    off = 0;
    int    mapIdx;
    int    kinds;
    int    i;
    int    emitted = 0;

    if (!(g_featureFlags & SHNET_SF_MEMOS))
    {
        return;
    }
    if (len < 8)
    {
        return;
    }
    mapIdx = (int)ShnGetU8(p, &off);
    kinds  = (int)ShnGetU8(p, &off);

    if (mapIdx < 0 || mapIdx >= SRV_MAP_COUNT)
    {
        return;
    }

    i = 0;
    do
    {
        int o     = SHNET_HDR_SIZE;
        int count = 0;
        int hdrAt = o;

        ShnPutU8(buf, &o, (shn_u8)mapIdx);
        ShnPutU8(buf, &o, 0);
        ShnPutU16(buf, &o, 0); /* count, back-patched */
        ShnPutU32(buf, &o, 0);

        for (; i < g_memoCount && count < SHNET_MEMO_MAX; i++)
        {
            const Memo* m = &g_memos[i];
            if (m->mapIdx != mapIdx)
            {
                continue;
            }
            if (!(kinds & (1 << (m->kind & 0x0F))) && kinds != 0xFF) /* mask the emote nibble */
            {
                continue;
            }
            ShnPutU32(buf, &o, m->memoId);
            ShnPutU32(buf, &o, m->ownerId);
            ShnPutU8(buf, &o, (shn_u8)m->kind);
            ShnPutU8(buf, &o, (shn_u8)m->mapIdx);
            ShnPutS16(buf, &o, m->rating);
            ShnPutU16(buf, &o, m->phraseA);
            ShnPutU16(buf, &o, m->wordA);
            ShnPutU16(buf, &o, m->phraseB);
            ShnPutU16(buf, &o, m->wordB);
            ShnPutS32(buf, &o, m->x);
            ShnPutS32(buf, &o, m->y);
            ShnPutS32(buf, &o, m->z);
            ShnPutS16(buf, &o, m->rotY);
            ShnPutStr(buf, &o, m->ownerName, SHNET_NAME_MAX);
            count++;
        }

        {
            int patch = hdrAt + 2;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        SrvSendTo(c, buf, o - SHNET_HDR_SIZE, SHNET_MSG_MEMO_LIST);
        emitted += count;

        /* An empty list still goes out once: the client is retrying until it
         * hears something, and "this map has no markers" is an answer. */
        if (count == 0)
        {
            break;
        }
    } while (i < g_memoCount);

    if (g_verbose)
    {
        SrvLog("  %s queried map %d -> %d markers", c->name, mapIdx, emitted);
    }
}

/* Live markers: push a just-placed memo to everyone else on that map now, so it
 * appears without waiting for their next map-entry query. Reuses the MEMO_LIST
 * wire (one entry); the client merges by memoId. */
static void SrvPushMemo(const Memo* m)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* o = &g_clients[i];
        shn_u8  buf[SHNET_HDR_SIZE + 8 + SHNET_MEMO_ENTRY_NAMED];
        int     bo = SHNET_HDR_SIZE;
        if (!o->used || o->mapIdx != m->mapIdx || SRV_IDENT(o) == m->ownerId)
        {
            continue;
        }
        ShnPutU8(buf, &bo, (shn_u8)m->mapIdx);
        ShnPutU8(buf, &bo, 0);
        ShnPutU16(buf, &bo, 1); /* one entry */
        ShnPutU32(buf, &bo, 0);
        ShnPutU32(buf, &bo, m->memoId);
        ShnPutU32(buf, &bo, m->ownerId);
        ShnPutU8(buf, &bo, (shn_u8)m->kind);
        ShnPutU8(buf, &bo, (shn_u8)m->mapIdx);
        ShnPutS16(buf, &bo, m->rating);
        ShnPutU16(buf, &bo, m->phraseA);
        ShnPutU16(buf, &bo, m->wordA);
        ShnPutU16(buf, &bo, m->phraseB);
        ShnPutU16(buf, &bo, m->wordB);
        ShnPutS32(buf, &bo, m->x);
        ShnPutS32(buf, &bo, m->y);
        ShnPutS32(buf, &bo, m->z);
        ShnPutS16(buf, &bo, m->rotY);
        ShnPutStr(buf, &bo, m->ownerName, SHNET_NAME_MAX);
        SrvSendTo(o, buf, bo - SHNET_HDR_SIZE, SHNET_MSG_MEMO_LIST);
    }
}

static void SrvHandleMemoPlace(Client* c, const shn_u8* p, int len)
{
    int     off = 0;
    Memo    m;
    shn_u8  ack[SHNET_HDR_SIZE + 8];
    int     ao = SHNET_HDR_SIZE;
    int     mine = 0;
    int     i;
    shn_u32 ip;

    if (!(g_featureFlags & SHNET_SF_MEMOS))
    {
        return;
    }
    if (len < 28)
    {
        return;
    }

    memset(&m, 0, sizeof(m));
    m.kind    = (int)ShnGetU8(p, &off);
    m.mapIdx  = (int)ShnGetU8(p, &off);
    m.phraseA = ShnGetU16(p, &off);
    m.wordA   = ShnGetU16(p, &off);
    m.phraseB = ShnGetU16(p, &off);
    m.wordB   = ShnGetU16(p, &off);
    (void)ShnGetU16(p, &off);
    m.x       = ShnGetS32(p, &off);
    m.y       = ShnGetS32(p, &off);
    m.z       = ShnGetS32(p, &off);
    m.rotY    = ShnGetS16(p, &off);

    if (m.mapIdx < 0 || m.mapIdx >= SRV_MAP_COUNT)
    {
        return;
    }
    /* kind byte: low nibble = marker kind, high nibble = emote (memos only). */
    if ((m.kind & 0x0F) > SHNET_MARK_SAVE)
    {
        return;
    }
    if ((m.kind & 0x0F) != SHNET_MARK_MEMO && (m.kind & 0xF0) != 0)
    {
        return;
    }
    if ((m.kind & 0x0F) == SHNET_MARK_DEATH && !(g_featureFlags & SHNET_SF_DEATHS))
    {
        return;
    }
    if (!SrvCoordOk(m.x) || !SrvCoordOk(m.y) || !SrvCoordOk(m.z))
    {
        return;
    }
    /* Template indices are persisted and replayed to every later visitor, so
     * an out-of-table one would be a stored attack on clients that index with
     * it. Only memos use them; the other kinds carry none. */
    if ((m.kind & 0x0F) == SHNET_MARK_MEMO)
    {
        if (m.phraseA >= SHNET_MEMO_PHRASE_MAX || m.wordA >= SHNET_MEMO_WORD_MAX)
        {
            return;
        }
        if (m.phraseB != SHNET_MEMO_NONE && m.phraseB >= SHNET_MEMO_PHRASE_MAX)
        {
            return;
        }
        if (m.wordB != SHNET_MEMO_NONE && m.wordB >= SHNET_MEMO_WORD_MAX)
        {
            return;
        }
    }
    else
    {
        m.phraseA = m.wordA = m.phraseB = m.wordB = SHNET_MEMO_NONE;
    }
    /* Deaths are exempt: they are already one per death by the dedupe below,
     * and dying twice in five seconds is a real thing that happens. */
    if ((m.kind & 0x0F) != SHNET_MARK_DEATH)
    {
        unsigned int now = SrvMillis();
        if (c->memoPlaced && (now - c->lastMemoMs) < SRV_MEMO_GAP_MS)
        {
            return;
        }
    }

    /* The client latches its death report, but a reconnect or a replayed
     * datagram can still produce a second one; collapse those here so a single
     * death never leaves a cluster of markers. */
    if ((m.kind & 0x0F) == SHNET_MARK_DEATH)
    {
        unsigned int now = SrvMillis();
        shn_s32      dx  = m.x - c->lastDeathX;
        shn_s32      dz  = m.z - c->lastDeathZ;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        if (c->lastDeathMs && (now - c->lastDeathMs) < SRV_DEATH_DEDUPE_MS &&
            dx < (SRV_DEATH_DEDUPE_DIST) && dz < (SRV_DEATH_DEDUPE_DIST))
        {
            return;
        }
        c->lastDeathMs = now;
        c->lastDeathX  = m.x;
        c->lastDeathZ  = m.z;
    }

    /* Keyed on the address, not the playerId: a playerId is new on every
     * HELLO, so a per-id cap was reset by simply reconnecting. */
    ip = c->addr.sin_addr.s_addr;
    for (i = 0; i < g_memoCount; i++)
    {
        if (g_memos[i].ownerIp == ip) mine++;
    }
    if (mine >= SRV_MEMOS_PER_USER)
    {
        /* Retire this address's oldest rather than refusing: a player who hit
         * the cap should still be able to leave a message. */
        int oldest = -1;
        for (i = 0; i < g_memoCount; i++)
        {
            if (g_memos[i].ownerIp != ip)
            {
                continue;
            }
            if (oldest < 0 || g_memos[i].placedUnix < g_memos[oldest].placedUnix)
            {
                oldest = i;
            }
        }
        if (oldest >= 0)
        {
            g_memos[oldest] = g_memos[--g_memoCount];
        }
    }

    while (SrvMemosOnMap(m.mapIdx) >= SRV_MEMOS_PER_MAP)
    {
        SrvEvictOnMap(m.mapIdx);
    }
    if (g_memoCount >= SRV_MAX_MEMOS)
    {
        SrvEvictOnMap(m.mapIdx);
    }
    if (g_memoCount >= SRV_MAX_MEMOS)
    {
        return;
    }

    m.memoId     = g_nextMemoId++;
    m.ownerId    = SRV_IDENT(c);
    m.ownerIp    = ip;
    m.rating     = 0;
    m.placedUnix = (long)time(NULL);
    memcpy(m.ownerName, c->name, SHNET_NAME_MAX);

    g_memos[g_memoCount++] = m;
    g_dbDirty = 1;
    if ((m.kind & 0x0F) != SHNET_MARK_DEATH)
    {
        c->lastMemoMs = SrvMillis();
        c->memoPlaced = 1;
    }

    ShnPutU32(ack, &ao, m.memoId);
    ShnPutU32(ack, &ao, (shn_u32)m.mapIdx);
    SrvSendTo(c, ack, ao - SHNET_HDR_SIZE, SHNET_MSG_MEMO_ACK);

    SrvPushMemo(&m); /* live: everyone else on this map sees it now */

    SrvLog("  %s placed %s on map %d (id %u)", c->name,
           (m.kind & 0x0F) == SHNET_MARK_DEATH ? "a death marker" :
           (m.kind & 0x0F) == SHNET_MARK_SAVE  ? "a save mark" : "a message",
           m.mapIdx, m.memoId);

    if ((m.kind & 0x0F) == SHNET_MARK_DEATH)
    {
        char text[SHNET_EVENT_MAX];
        snprintf(text, sizeof(text), "%s died", c->name);
        SrvBroadcastEvent(SHNET_EV_DEATH, c->playerId, text, c);
    }
}

/* One vote per (address, memo). A small set-associative table rather than an
 * exact record: it forgets old votes once it fills, which bounds memory and
 * still turns a vote flood into one vote per memo per address. */
static int SrvVoteOnce(shn_u32 ip, shn_u32 memoId)
{
    shn_u8  key[8];
    int     b, w;
    Vote*   set;

    memcpy(key, &ip, 4);
    memcpy(key + 4, &memoId, 4);
    b   = (int)(ShnSipHash(g_cookieKey, key, 8) % SRV_VOTE_BUCKETS);
    set = g_votes[b];
    for (w = 0; w < SRV_VOTE_WAYS; w++)
    {
        if (set[w].memoId == memoId && set[w].ip == ip)
        {
            return 0;
        }
    }
    w = g_voteNext[b];
    g_voteNext[b] = (unsigned char)((w + 1) % SRV_VOTE_WAYS);
    set[w].ip     = ip;
    set[w].memoId = memoId;
    return 1;
}

static void SrvHandleMemoRate(Client* c, const shn_u8* p, int len)
{
    int     off = 0;
    shn_u32 id;
    int     delta;
    int     i;

    if (len < 8)
    {
        return;
    }
    id    = ShnGetU32(p, &off);
    delta = (int)(signed char)ShnGetU8(p, &off);

    for (i = 0; i < g_memoCount; i++)
    {
        int r;
        if (g_memos[i].memoId != id)
        {
            continue;
        }
        if (g_memos[i].ownerId == SRV_IDENT(c))
        {
            return; /* no self-rating */
        }
        /* You rate what you are reading, which means standing on its map;
         * this also stops one client sweeping ratings across the whole store. */
        if (c->mapIdx != g_memos[i].mapIdx)
        {
            return;
        }
        if (!SrvVoteOnce(c->addr.sin_addr.s_addr, id))
        {
            return;
        }
        r = (int)g_memos[i].rating + (delta >= 0 ? 1 : -1);
        if (r >  SRV_RATING_CLAMP) r =  SRV_RATING_CLAMP;
        if (r < -SRV_RATING_CLAMP) r = -SRV_RATING_CLAMP;
        g_memos[i].rating = (shn_s16)r;
        g_dbDirty = 1;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Snapshots                                                           */
/* ------------------------------------------------------------------ */

/* Position fan-out is filtered by MAP on the server rather than by distance:
 * the server has no collision or geometry, so it cannot know what is visible,
 * and the client's own online_ghost_range already handles the near case. Map
 * filtering is what actually matters — it keeps a 60-player server's traffic
 * proportional to the people you could plausibly meet.
 *
 * One datagram holds SHNET_SNAP_MAX peers. When a map has more than that, the
 * NEAREST ones go out: in slot order a player standing next to you could be
 * the one left off, while the whole-town view makes far players visible too,
 * so the cut has to fall on the far end. */
typedef struct
{
    int                idx;
    unsigned long long d2;
} SnapPick;

static int SrvSnapPickCmp(const void* a, const void* b)
{
    const SnapPick* pa = (const SnapPick*)a;
    const SnapPick* pb = (const SnapPick*)b;
    return (pa->d2 > pb->d2) - (pa->d2 < pb->d2);
}

static void SrvSendSnapshots(unsigned int now)
{
    static SnapPick picks[SRV_MAX_CLIENTS];
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* c = &g_clients[i];
        shn_u8  buf[SHNET_MTU];
        int     off   = SHNET_HDR_SIZE;
        int     count = 0;
        int     hdrAt;
        int     j;

        if (!c->used || c->mapIdx < 0)
        {
            continue;
        }
        if (now - c->lastSnapMs < SRV_TICK_MS)
        {
            continue;
        }
        c->lastSnapMs = now;

        if (!(g_featureFlags & SHNET_SF_GHOSTS))
        {
            continue;
        }

        hdrAt = off;
        ShnPutU32(buf, &off, now);
        ShnPutU16(buf, &off, 0); /* count, back-patched */
        ShnPutU16(buf, &off, 0);

        {
            int n = 0;
            for (j = 0; j < SRV_MAX_CLIENTS; j++)
            {
                Client* o = &g_clients[j];
                long long dx, dz;
                unsigned long long ux, uz;
                if (!o->used || o == c || o->mapIdx != c->mapIdx)
                {
                    continue;
                }
                /* Coordinates are clamped on receipt, so |d| < 2^25 and the
                 * squares cannot overflow; squaring unsigned keeps it defined
                 * even if that clamp is ever loosened. */
                dx = (long long)o->x - c->x;
                dz = (long long)o->z - c->z;
                ux = (unsigned long long)(dx < 0 ? -dx : dx);
                uz = (unsigned long long)(dz < 0 ? -dz : dz);
                picks[n].idx = j;
                picks[n].d2  = ux * ux + uz * uz;
                n++;
            }
            if (n > SHNET_SNAP_MAX)
            {
                qsort(picks, (size_t)n, sizeof(picks[0]), SrvSnapPickCmp);
                n = SHNET_SNAP_MAX;
            }
            picks[n].idx = -1;
        }

        for (j = 0; picks[j].idx >= 0; j++)
        {
            Client* o = &g_clients[picks[j].idx];
            ShnPutU32(buf, &off, o->playerId);
            ShnPutU8(buf, &off, (shn_u8)o->charaId);
            ShnPutU8(buf, &off, (shn_u8)o->flags);
            ShnPutS32(buf, &off, o->x);
            ShnPutS32(buf, &off, o->y);
            ShnPutS32(buf, &off, o->z);
            ShnPutS16(buf, &off, o->rotY);
            ShnPutU16(buf, &off, o->anim);
            ShnPutU16(buf, &off, o->frame);
            count++;
        }

        /* Nothing to say: skip the datagram entirely. The client's stale
         * timer removes ghosts on its own, so silence is a valid message and
         * an empty map costs zero bandwidth. */
        if (count == 0)
        {
            continue;
        }

        {
            int patch = hdrAt + 4;
            ShnPutU16(buf, &patch, (shn_u16)count);
        }
        if (g_verbose && !c->loggedSnap)
        {
            c->loggedSnap = 1;
            SrvLog("  -> first snapshot to %s: %d peer(s) on map %d, %d bytes",
                   c->name, count, c->mapIdx, SHNET_HDR_SIZE + off - SHNET_HDR_SIZE);
        }
        SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_SNAPSHOT);
    }
}

static void SrvRoomLeave(Client* c, int notify); /* defined with the co-op rooms below */

static void SrvExpire(unsigned int now)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* c = &g_clients[i];
        char    text[SHNET_EVENT_MAX];

        if (!c->used || (now - c->lastRxMs) < SRV_TIMEOUT_MS)
        {
            continue;
        }
        snprintf(text, sizeof(text), "%s faded away", c->name);
        SrvLog("- %s timed out (id %u)", c->name, c->playerId);
        SrvRoomLeave(c, 1); /* notify the room before the slot is freed */
        c->used = 0;
        SrvBroadcastEvent(SHNET_EV_LEAVE, c->playerId, text, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Co-op rooms (relay)                                                 */
/* ------------------------------------------------------------------ */

#define SRV_ROOM_MAX_PLAYERS 8

static Client* SrvRoomHost(shn_u16 room)
{
    int i;
    if (!room) return NULL;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].room == room && g_clients[i].roomHost)
            return &g_clients[i];
    return NULL;
}

static int SrvRoomCount(shn_u16 room)
{
    int i, n = 0;
    if (!room) return 0;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].room == room) n++;
    return n;
}

/* ROOM_JOINED (to the one who just joined, yourId prefixed) or ROOM_ROSTER
 * (to the rest). hostId 0 means the room has ended. */
static void SrvSendRoomRoster(Client* c, int joined)
{
    shn_u8  buf[SHNET_MTU];
    int     off = SHNET_HDR_SIZE, cntOff, i, n = 0;
    Client* host   = SrvRoomHost(c->room);
    shn_u32 hostId = host ? host->playerId : 0;

    if (joined) ShnPutU32(buf, &off, c->playerId);
    ShnPutU16(buf, &off, c->room);
    ShnPutU32(buf, &off, hostId);
    cntOff = off; ShnPutU8(buf, &off, 0);
    /* room 0 is "not in a room", which every un-roomed player shares; listing
     * it would hand a kicked member the names of strangers server-wide. */
    for (i = 0; c->room && i < SRV_MAX_CLIENTS; i++)
    {
        Client* m = &g_clients[i];
        if (!m->used || m->room != c->room) continue;
        if (off + 4 + SHNET_NAME_MAX + 1 > (int)sizeof(buf)) break;
        ShnPutU32(buf, &off, m->playerId);
        ShnPutStr(buf, &off, m->name, SHNET_NAME_MAX);
        ShnPutU8(buf, &off, (shn_u8)(m->roomHost ? 1 : 0));
        n++;
    }
    buf[cntOff] = (shn_u8)n;
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE,
              joined ? SHNET_MSG_ROOM_JOINED : SHNET_MSG_ROOM_ROSTER);
}

static void SrvBroadcastRoomRoster(shn_u16 room, Client* except)
{
    int i;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].room == room && &g_clients[i] != except)
            SrvSendRoomRoster(&g_clients[i], 0);
}

static void SrvSendRoomReject(Client* c, int reason, const char* text)
{
    shn_u8 buf[SHNET_HDR_SIZE + 1 + SHNET_REJECT_MAX];
    int    off = SHNET_HDR_SIZE;
    ShnPutU8(buf, &off, (shn_u8)reason);
    ShnPutStr(buf, &off, text ? text : "", SHNET_REJECT_MAX);
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_ROOM_REJECT);
}

static shn_u16 SrvNewRoomCode(void)
{
    int tries;
    for (tries = 0; tries < 10000; tries++)
    {
        shn_u32 r;
        shn_u16 code;
        SrvRandomBytes(&r, (int)sizeof(r));
        code = (shn_u16)(1000 + (r % 9000)); /* shareable 4-digit */
        if (SrvRoomCount(code) == 0) return code;
    }
    return 0;
}

/* Leave the current room. On the host leaving, the room ends: every member is
 * cleared and told (roster with host 0). */
static void SrvRoomLeave(Client* c, int notify)
{
    shn_u16 room    = c->room;
    int     wasHost = c->roomHost;
    if (!room) return;
    c->room = 0; c->roomHost = 0; c->roomMax = 0; c->roomHidden = 0;
    if (!notify) return;
    if (wasHost)
    {
        int i;
        for (i = 0; i < SRV_MAX_CLIENTS; i++)
        {
            Client* m = &g_clients[i];
            if (m->used && m->room == room)
            {
                m->room = 0; m->roomHost = 0; m->roomMax = 0; m->roomHidden = 0;
                SrvSendRoomRoster(m, 0);
            }
        }
        SrvLog("- room %u closed", room);
    }
    else
    {
        SrvBroadcastRoomRoster(room, NULL);
    }
}

static void SrvHandleRoomCreate(Client* c, const shn_u8* p, int len)
{
    int     off    = 0;
    int     maxP   = (len >= 1) ? (int)ShnGetU8(p, &off) : 4;
    int     hidden = (len >= 2) ? (int)ShnGetU8(p, &off) : 0;
    shn_u16 code;
    if (maxP < 2) maxP = 2;
    if (maxP > SRV_ROOM_MAX_PLAYERS) maxP = SRV_ROOM_MAX_PLAYERS;
    SrvRoomLeave(c, 1);
    code = SrvNewRoomCode();
    if (!code) { SrvSendRoomReject(c, 1, "server is out of room codes"); return; }
    c->room = code; c->roomHost = 1; c->roomMax = maxP; c->roomHidden = hidden ? 1 : 0;
    SrvLog("+ room %u opened by %s (id %u, max %d%s)", code, c->name, c->playerId, maxP,
           c->roomHidden ? ", hidden" : "");
    SrvSendRoomRoster(c, 1);
}

static void SrvRoomJoinFailed(Client* c, unsigned int now)
{
    if (++c->joinFails >= SRV_JOIN_FAILS_MAX)
    {
        c->joinBlocked      = 1;
        c->joinBlockUntilMs = now + SRV_JOIN_BLOCK_MS;
        SrvLog("  %s (id %u) missed %d room joins; ignoring joins for %d s",
               c->name, c->playerId, c->joinFails, SRV_JOIN_BLOCK_MS / 1000);
    }
}

static int SrvSameBuild(const Client* a, const Client* b)
{
    return strncmp(a->build, b->build, SHNET_BUILD_MAX) == 0;
}

static void SrvHandleRoomJoin(Client* c, const shn_u8* p, int len)
{
    int     off  = 0;
    shn_u16 code = (len >= 2) ? (shn_u16)ShnGetU16(p, &off) : 0;
    Client* host;
    unsigned int now = SrvMillis();

    /* 9000 codes fall to a patient guesser; past a run of misses this client
     * goes quiet for a minute, which puts a sweep at hours, not seconds. */
    if (c->joinBlocked)
    {
        if ((int)(now - c->joinBlockUntilMs) < 0)
        {
            return;
        }
        c->joinBlocked = 0;
        c->joinFails   = 0;
    }
    /* Code 0 means "whatever room is open here" -- the Join-by-IP path, which
     * reaches a host directly and need not know a code. Take the first open
     * one the host chose to list; a hidden room is joined by its code only. */
    if (code == 0)
    {
        int i, otherBuild = 0;
        for (i = 0; i < SRV_MAX_CLIENTS; i++)
        {
            Client* h = &g_clients[i];
            if (h->used && h->roomHost && !h->roomHidden && SrvRoomCount(h->room) < h->roomMax)
            {
                if (!SrvSameBuild(h, c)) { otherBuild = 1; continue; }
                code = h->room;
                break;
            }
        }
        if (code == 0 && otherBuild)
        {
            SrvSendRoomReject(c, 5, "The host is on a different game build. Update both games to the newest build.");
            return;
        }
        if (code == 0) { SrvRoomJoinFailed(c, now); SrvSendRoomReject(c, 2, "no open room here"); return; }
    }
    host = SrvRoomHost(code);
    if (!host)                               { SrvRoomJoinFailed(c, now); SrvSendRoomReject(c, 2, "no such room"); return; }
    {
        int k;
        for (k = 0; k < host->kickedN; k++)
        {
            if (host->kickedIp[k].s_addr == c->addr.sin_addr.s_addr)
            {
                SrvSendRoomReject(c, 4, "removed by the host");
                return;
            }
        }
    }
    if (SrvRoomCount(code) >= host->roomMax) { SrvRoomJoinFailed(c, now); SrvSendRoomReject(c, 3, "room is full"); return; }
    if (!SrvSameBuild(host, c))
    {
        SrvLog("  %s (id %u, build %s) refused room %u: host is on build %s",
               c->name, c->playerId, c->build, code, host->build);
        SrvSendRoomReject(c, 5, "The host is on a different game build. Update both games to the newest build.");
        return;
    }
    SrvRoomLeave(c, 1);
    c->room = code; c->roomHost = 0; c->roomMax = host->roomMax;
    SrvLog("  %s (id %u) joined room %u", c->name, c->playerId, code);
    SrvSendRoomRoster(c, 1);
    SrvBroadcastRoomRoster(code, c);
}

/* The room host removes a member: out of the room on the server's word, so a
 * modified client cannot stay by ignoring the host's S_KICK, and its address
 * is refused on this room until the host closes it. */
static void SrvHandleRoomKick(Client* c, const shn_u8* p, int len)
{
    int     off = 0, i;
    shn_u32 who;
    if (!c->room || !c->roomHost || len < 4) return;
    who = ShnGetU32(p, &off);
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* m = &g_clients[i];
        if (!m->used || m == c || m->room != c->room || m->playerId != who) continue;
        if (c->kickedN < (int)(sizeof(c->kickedIp) / sizeof(c->kickedIp[0])))
        {
            c->kickedIp[c->kickedN++] = m->addr.sin_addr;
        }
        SrvLog("  %s removed %s (id %u) from room %u", c->name, m->name, m->playerId, c->room);
        SrvRoomLeave(m, 1);
        SrvSendRoomReject(m, 4, "removed by the host");
        break;
    }
}

static void SrvHandleRoomList(Client* c)
{
    shn_u8 buf[SHNET_MTU];
    int    off = SHNET_HDR_SIZE, cntOff, i, n = 0;
    cntOff = off; ShnPutU8(buf, &off, 0);
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* h = &g_clients[i];
        if (!h->used || !h->roomHost || h->roomHidden) continue;
        if (off + 2 + 1 + 1 + SHNET_NAME_MAX + 1 > (int)sizeof(buf)) break;
        ShnPutU16(buf, &off, h->room);
        ShnPutU8(buf, &off, (shn_u8)SrvRoomCount(h->room));
        ShnPutU8(buf, &off, (shn_u8)h->roomMax);
        ShnPutStr(buf, &off, h->name, SHNET_NAME_MAX);
        /* The host's map (from its living-world state), so the browser can say
         * where the game is; 0xFF when the host has not reported one. */
        ShnPutU8(buf, &off, (shn_u8)((h->mapIdx < 0) ? 0xFF : h->mapIdx));
        if (++n >= 40) break;
    }
    buf[cntOff] = (shn_u8)n;
    SrvSendTo(c, buf, off - SHNET_HDR_SIZE, SHNET_MSG_ROOM_LIST);
}

/* Forward a member's co-op packet to the rest of its room (target 0) or to one
 * member (target == their id). The payload is an S_* message relayed byte for
 * byte; the server checks only its framing and who may send it, so that what
 * arrives at a peer is at least a well-formed co-op packet. */
static void SrvHandleCoopRelay(Client* c, const shn_u8* p, int len)
{
    int           off = 0, innerLen, i;
    shn_u32       target;
    const shn_u8* inner;
    shn_u8        iType;
    shn_u16       iPay;
    shn_u32       iSess;
    if (!c->room || len < 4) return;
    target   = ShnGetU32(p, &off);
    inner    = p + off;
    innerLen = len - off;
    if (innerLen <= 0 || innerLen > SHNET_MTU - SHNET_HDR_SIZE - 4) return;
    if (!ShnParseHeader(inner, innerLen, &iType, &iPay, &iSess)) return;
    if (iType < SHNET_MSG_S_FIRST || iType > SHNET_MSG_S_LAST) return;
    if (SHNET_HDR_SIZE + (int)iPay != innerLen) return;
    /* S_WORLD boots every guest into a map, S_KICK removes one; only the host
     * may say either. */
    if (SHNET_S_HOST_ONLY(iType) && !c->roomHost) return;
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        Client* m = &g_clients[i];
        shn_u8  buf[SHNET_MTU];
        int     o = SHNET_HDR_SIZE;
        if (!m->used || m->room != c->room || m == c) continue;
        if (target != 0 && m->playerId != target) continue;
        ShnPutU32(buf, &o, c->playerId);
        memcpy(buf + o, inner, (size_t)innerLen); o += innerLen;
        SrvSendTo(m, buf, o - SHNET_HDR_SIZE, SHNET_MSG_COOP_RELAY);
    }
}

static void SrvDispatch(const struct sockaddr_in* from, const shn_u8* buf, int n)
{
    shn_u8       type;
    shn_u16      payLen;
    shn_u32      session;
    const shn_u8* pay;
    Client*      c;
    unsigned int now = SrvMillis();

    if (!ShnParseHeader(buf, n, &type, &payLen, &session))
    {
        return;
    }
    pay = buf + SHNET_HDR_SIZE;

    if (type == SHNET_MSG_HELLO)
    {
        SrvHandleHello(from, pay, (int)payLen);
        return;
    }

    /* Every other message needs a live session, and it has to have come from
     * the address that session was established on: a session id alone would
     * let anyone who saw one impersonate that player. */
    c = SrvFindBySession(session);
    if (!c || !SrvAddrEqual(&c->addr, from))
    {
        return;
    }

    if (now - c->rateWindowMs >= 1000u)
    {
        c->rateWindowMs = now;
        c->rateCount    = 0;
    }
    if (++c->rateCount > SRV_RATE_PER_SEC)
    {
        return;
    }

    c->lastRxMs = now;

#ifdef SH_MASTER_EMBED
    /* A This-PC co-op host is a relay for one room of friends, not a living
     * world: chat and the marker store have no business on it. */
    if (type == SHNET_MSG_CHAT_SAY || type == SHNET_MSG_MEMO_PLACE ||
        type == SHNET_MSG_MEMO_QUERY || type == SHNET_MSG_MEMO_RATE)
    {
        return;
    }
#endif

    switch (type)
    {
    case SHNET_MSG_STATE:      SrvHandleState(c, pay, (int)payLen);      break;
    case SHNET_MSG_PING:       SrvHandlePing(c, pay, (int)payLen);       break;
    case SHNET_MSG_ROSTER_REQ: SrvHandleRosterReq(c);                    break;
    case SHNET_MSG_MEMO_QUERY: SrvHandleMemoQuery(c, pay, (int)payLen);  break;
    case SHNET_MSG_MEMO_PLACE: SrvHandleMemoPlace(c, pay, (int)payLen);  break;
    case SHNET_MSG_MEMO_RATE:  SrvHandleMemoRate(c, pay, (int)payLen);   break;
    case SHNET_MSG_CHAT_SAY:   SrvHandleChatSay(c, pay, (int)payLen);    break;
    case SHNET_MSG_ROOM_CREATE:   SrvHandleRoomCreate(c, pay, (int)payLen); break;
    case SHNET_MSG_ROOM_JOIN:     SrvHandleRoomJoin(c, pay, (int)payLen);   break;
    case SHNET_MSG_ROOM_LEAVE:    SrvRoomLeave(c, 1);                       break;
    case SHNET_MSG_ROOM_LIST_REQ: SrvHandleRoomList(c);                     break;
    case SHNET_MSG_COOP_RELAY:    SrvHandleCoopRelay(c, pay, (int)payLen);  break;
    case SHNET_MSG_ROOM_KICK:     SrvHandleRoomKick(c, pay, (int)payLen);   break;
    case SHNET_MSG_BYE:
    {
        char text[SHNET_EVENT_MAX];
        snprintf(text, sizeof(text), "%s left", c->name);
        SrvLog("- %s left (id %u)", c->name, c->playerId);
        SrvRoomLeave(c, 1); /* notify the room before the slot is freed */
        c->used = 0;
        SrvBroadcastEvent(SHNET_EV_LEAVE, c->playerId, text, NULL);
        break;
    }
    default: break;
    }
}

#ifndef SH_MASTER_EMBED
/* ------------------------------------------------------------------ */
/* WebSocket clients (the browser build)                               */
/* ------------------------------------------------------------------ */

/* A browser cannot send UDP, so the web build sends the same datagrams as
 * WebSocket binary messages, one datagram per message, and everything above
 * this section never knows the difference. Each connection is given a
 * stand-in sockaddr_in: the player's IP, the slot's generation as the port,
 * and WS_TAG plus the slot index in sin_zero, which no address from recvfrom
 * carries (the UDP path zeroes it), so the two kinds can never be confused.
 *
 * TLS is a reverse proxy's job (a page served over https may only open
 * wss://). Behind one the peer is loopback, and the player's address comes
 * from the last X-Forwarded-For entry, the one the proxy itself appended.
 * That header is believed from loopback only: anyone else could write
 * whatever address they liked into it and walk past the per-address limits. */

#define WS_MAX_CONNS     SRV_MAX_CLIENTS
#define WS_RX_CAP        4096   /* the handshake, or a frame plus the next one's start */
#define WS_TX_CAP        65536
#define WS_HANDSHAKE_MS  5000
#define WS_IDLE_MS       30000  /* a live client pings every 2 s */
#define WS_TAG           0x57

typedef struct
{
    ShSock             fd;
    int                used;
    int                open;      /* the HTTP upgrade is done */
    unsigned short     gen;
    struct sockaddr_in addr;
    unsigned int       acceptMs;
    unsigned int       lastRxMs;
    int                rxLen;
    int                txLen;
    shn_u8             rx[WS_RX_CAP];
    shn_u8             tx[WS_TX_CAP];
} WsConn;

static WsConn          g_ws[WS_MAX_CONNS];
static ShSock          g_wsListen = SH_INVALID_SOCK;
static unsigned short  g_wsPort;

/* --- SHA-1, for the Sec-WebSocket-Accept answer and nothing else --- */

typedef struct { shn_u32 h[5]; shn_u8 blk[64]; unsigned long long len; int fill; } WsSha1Ctx;

static shn_u32 WsRol(shn_u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static void WsSha1Block(WsSha1Ctx* s, const shn_u8* p)
{
    shn_u32 w[80], a, b, c, d, e, f, k, t;
    int     i;
    for (i = 0; i < 16; i++)
        w[i] = ((shn_u32)p[i * 4] << 24) | ((shn_u32)p[i * 4 + 1] << 16) |
               ((shn_u32)p[i * 4 + 2] << 8) | (shn_u32)p[i * 4 + 3];
    for (i = 16; i < 80; i++) w[i] = WsRol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++)
    {
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = WsRol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = WsRol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void WsSha1Feed(WsSha1Ctx* s, const shn_u8* p, int n)
{
    while (n-- > 0)
    {
        s->blk[s->fill++] = *p++;
        s->len += 8;
        if (s->fill == 64) { WsSha1Block(s, s->blk); s->fill = 0; }
    }
}

static void WsSha1(const shn_u8* p, int n, shn_u8 out[20])
{
    WsSha1Ctx          s;
    unsigned long long bits;
    shn_u8             pad = 0x80, z = 0, lenBe[8];
    int                i;
    memset(&s, 0, sizeof(s));
    s.h[0] = 0x67452301u; s.h[1] = 0xEFCDAB89u; s.h[2] = 0x98BADCFEu;
    s.h[3] = 0x10325476u; s.h[4] = 0xC3D2E1F0u;
    WsSha1Feed(&s, p, n);
    bits = s.len;
    WsSha1Feed(&s, &pad, 1);
    while (s.fill != 56) WsSha1Feed(&s, &z, 1);
    for (i = 0; i < 8; i++) lenBe[i] = (shn_u8)(bits >> (56 - 8 * i));
    WsSha1Feed(&s, lenBe, 8);
    for (i = 0; i < 20; i++) out[i] = (shn_u8)(s.h[i / 4] >> (24 - 8 * (i % 4)));
}

static void WsBase64(const shn_u8* p, int n, char* out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    for (i = 0; i < n; i += 3)
    {
        shn_u32 v = (shn_u32)p[i] << 16;
        if (i + 1 < n) v |= (shn_u32)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = '\0';
}

/* --- connections --- */

static void WsSetNonBlocking(ShSock fd)
{
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static int WsWouldBlock(void)
{
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static int WsListen(unsigned short port)
{
    struct sockaddr_in a;
    int                one = 1;

    g_wsListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_wsListen == SH_INVALID_SOCK)
    {
        return 0;
    }
    setsockopt(g_wsListen, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port        = htons(port);
    if (bind(g_wsListen, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(g_wsListen, 64) != 0)
    {
        SH_CLOSESOCK(g_wsListen);
        g_wsListen = SH_INVALID_SOCK;
        return 0;
    }
    WsSetNonBlocking(g_wsListen);
    return 1;
}

static int WsFlush(WsConn* w)
{
    while (w->txLen > 0)
    {
#if defined(MSG_NOSIGNAL)
        int n = (int)send(w->fd, (const char*)w->tx, (size_t)w->txLen, MSG_NOSIGNAL);
#else
        int n = (int)send(w->fd, (const char*)w->tx, (size_t)w->txLen, 0);
#endif
        if (n <= 0)
        {
            return (n < 0 && WsWouldBlock()) ? 1 : 0;
        }
        memmove(w->tx, w->tx + n, (size_t)(w->txLen - n));
        w->txLen -= n;
    }
    return 1;
}

/* Queues one frame. A frame that does not fit is dropped whole, the same as
 * a datagram lost on the way: half of one would wreck the stream. */
static void WsQueueFrame(WsConn* w, int opcode, const shn_u8* p, int n)
{
    int hdr = (n < 126) ? 2 : 4;
    if (w->txLen + hdr + n > WS_TX_CAP)
    {
        return;
    }
    w->tx[w->txLen++] = (shn_u8)(0x80 | opcode);
    if (n < 126)
    {
        w->tx[w->txLen++] = (shn_u8)n;
    }
    else
    {
        w->tx[w->txLen++] = 126;
        w->tx[w->txLen++] = (shn_u8)(n >> 8);
        w->tx[w->txLen++] = (shn_u8)n;
    }
    if (n > 0)
    {
        memcpy(w->tx + w->txLen, p, (size_t)n);
        w->txLen += n;
    }
}

static void SrvDispatch(const struct sockaddr_in* from, const shn_u8* buf, int n);

static void WsClose(WsConn* w)
{
    if (!w->used)
    {
        return;
    }
    if (w->open)
    {
        /* A closed tab is a goodbye: say so for the player rather than leave
         * their room waiting out the 15 s timeout. */
        Client* c = SrvFindByAddr(&w->addr);
        if (c)
        {
            shn_u8 bye[SHNET_HDR_SIZE];
            ShnPutHeader(bye, SHNET_MSG_BYE, 0, c->session);
            SrvDispatch(&w->addr, bye, SHNET_HDR_SIZE);
        }
    }
    SH_CLOSESOCK(w->fd);
    w->fd    = SH_INVALID_SOCK;
    w->used  = 0;
    w->open  = 0;
    w->rxLen = 0;
    w->txLen = 0;
    w->gen++;
}

static void WsSend(const struct sockaddr_in* to, const shn_u8* buf, int len)
{
    int     slot = to->sin_zero[1] | (to->sin_zero[2] << 8);
    WsConn* w;
    if (slot >= WS_MAX_CONNS)
    {
        return;
    }
    w = &g_ws[slot];
    if (!w->used || !w->open || htons(w->gen) != to->sin_port)
    {
        return;
    }
    WsQueueFrame(w, 0x2, buf, len);
    if (!WsFlush(w))
    {
        WsClose(w);
    }
}

static void WsAccept(unsigned int now)
{
    for (;;)
    {
        struct sockaddr_in peer;
        ShSockLen          plen = (ShSockLen)sizeof(peer);
        ShSock             fd   = accept(g_wsListen, (struct sockaddr*)&peer, &plen);
        int                i;
        if (fd == SH_INVALID_SOCK)
        {
            return;
        }
        for (i = 0; i < WS_MAX_CONNS && g_ws[i].used; i++)
        {
        }
        if (i == WS_MAX_CONNS)
        {
            SH_CLOSESOCK(fd);
            continue;
        }
        WsSetNonBlocking(fd);
        {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        }
        g_ws[i].fd       = fd;
        g_ws[i].used     = 1;
        g_ws[i].open     = 0;
        g_ws[i].rxLen    = 0;
        g_ws[i].txLen    = 0;
        g_ws[i].acceptMs = now;
        g_ws[i].lastRxMs = now;
        memset(&g_ws[i].addr, 0, sizeof(g_ws[i].addr));
        g_ws[i].addr.sin_family = AF_INET;
        g_ws[i].addr.sin_addr   = peer.sin_addr;
        g_ws[i].addr.sin_port   = htons(g_ws[i].gen);
        g_ws[i].addr.sin_zero[0] = WS_TAG;
        g_ws[i].addr.sin_zero[1] = (unsigned char)(i & 0xFF);
        g_ws[i].addr.sin_zero[2] = (unsigned char)(i >> 8);
    }
}

/* The value of header `name` (case-insensitive) in the request, trimmed. */
static int WsHeader(const char* req, const char* name, char* out, int cap)
{
    const char* p   = req;
    size_t      nl  = strlen(name);
    while ((p = strstr(p, "\r\n")) != NULL)
    {
        p += 2;
        if (!strncmp(p, "\r\n", 2))
        {
            break;
        }
        {
            size_t i;
            for (i = 0; i < nl; i++)
            {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
            }
            if (i == nl && p[nl] == ':')
            {
                const char* v = p + nl + 1;
                int         o = 0;
                while (*v == ' ' || *v == '\t') v++;
                while (*v && *v != '\r' && o < cap - 1) out[o++] = *v++;
                while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
                out[o] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Returns 0 when the request is not a WebSocket upgrade. */
static int WsHandshake(WsConn* w, char* req)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char   key[128], cat[192], accept[40], resp[256], fwd[256];
    shn_u8 dig[20];
    int    n;

    if (strncmp(req, "GET ", 4) != 0 || !WsHeader(req, "Sec-WebSocket-Key", key, (int)sizeof(key)) ||
        !key[0] || strlen(key) > 64)
    {
        return 0;
    }
    if (w->addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
        WsHeader(req, "X-Forwarded-For", fwd, (int)sizeof(fwd)))
    {
        const char*   last = strrchr(fwd, ',');
        unsigned long ip;
        last = last ? last + 1 : fwd;
        while (*last == ' ') last++;
        ip = inet_addr(last);
        if (ip != INADDR_NONE)
        {
            w->addr.sin_addr.s_addr = (shn_u32)ip;
        }
    }
    snprintf(cat, sizeof(cat), "%s%s", key, guid);
    WsSha1((const shn_u8*)cat, (int)strlen(cat), dig);
    WsBase64(dig, 20, accept);
    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (w->txLen + n > WS_TX_CAP)
    {
        return 0;
    }
    memcpy(w->tx + w->txLen, resp, (size_t)n);
    w->txLen += n;
    w->open = 1;
    if (g_verbose)
    {
        char a[48];
        SrvAddrStr(&w->addr, a, (int)sizeof(a));
        SrvLog("websocket open from %s", a);
    }
    return 1;
}

/* Handles every whole frame in rx. Returns 0 when the connection must close. */
static int WsFrames(WsConn* w)
{
    int pos = 0;
    while (w->rxLen - pos >= 2)
    {
        const shn_u8* f      = w->rx + pos;
        int           fin    = f[0] & 0x80;
        int           opcode = f[0] & 0x0F;
        int           masked = f[1] & 0x80;
        int           len    = f[1] & 0x7F;
        int           hdr    = 2;
        shn_u8        payload[SHNET_MTU];
        int           i;

        /* Clients always mask (RFC 6455 5.1); our datagrams always fit one
         * unfragmented frame, and anything larger is not a datagram. */
        if (!masked || (f[0] & 0x70))
        {
            return 0;
        }
        if (len == 126)
        {
            if (w->rxLen - pos < 4) break;
            len = (f[2] << 8) | f[3];
            hdr = 4;
        }
        else if (len == 127)
        {
            return 0;
        }
        if (len > SHNET_MTU)
        {
            return 0;
        }
        if (w->rxLen - pos < hdr + 4 + len)
        {
            break;
        }
        for (i = 0; i < len; i++)
        {
            payload[i] = (shn_u8)(f[hdr + 4 + i] ^ f[hdr + (i & 3)]);
        }
        pos += hdr + 4 + len;

        if (opcode == 0x8)
        {
            WsQueueFrame(w, 0x8, NULL, 0);
            WsFlush(w);
            return 0;
        }
        if (opcode == 0x9)
        {
            WsQueueFrame(w, 0xA, payload, len);
            continue;
        }
        if (opcode == 0x2)
        {
            if (!fin) return 0;
            SrvDispatch(&w->addr, payload, len);
            if (!w->used) return 1; /* the dispatch closed it */
            continue;
        }
        if (opcode == 0x0)
        {
            return 0;
        }
        /* text and pong: nothing to do */
    }
    if (pos > 0)
    {
        memmove(w->rx, w->rx + pos, (size_t)(w->rxLen - pos));
        w->rxLen -= pos;
    }
    return 1;
}

static void WsRead(WsConn* w, unsigned int now)
{
    for (;;)
    {
        int n;
        if (w->rxLen >= WS_RX_CAP)
        {
            WsClose(w);
            return;
        }
        n = (int)recv(w->fd, (char*)w->rx + w->rxLen, (size_t)(WS_RX_CAP - w->rxLen), 0);
        if (n == 0 || (n < 0 && !WsWouldBlock()))
        {
            WsClose(w);
            return;
        }
        if (n < 0)
        {
            break;
        }
        w->rxLen   += n;
        w->lastRxMs = now;

        if (!w->open)
        {
            char* end;
            w->rx[w->rxLen < WS_RX_CAP ? w->rxLen : WS_RX_CAP - 1] = 0;
            end = strstr((char*)w->rx, "\r\n\r\n");
            if (!end)
            {
                continue;
            }
            end[2] = '\0';
            if (!WsHandshake(w, (char*)w->rx))
            {
                static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                send(w->fd, bad, (int)sizeof(bad) - 1, 0);
                WsClose(w);
                return;
            }
            {
                int used = (int)(end + 4 - (char*)w->rx);
                memmove(w->rx, w->rx + used, (size_t)(w->rxLen - used));
                w->rxLen -= used;
            }
        }
        if (!WsFrames(w))
        {
            WsClose(w);
            return;
        }
        if (!w->used)
        {
            return;
        }
    }
    if (!WsFlush(w))
    {
        WsClose(w);
    }
}

static void WsExpire(unsigned int now)
{
    int i;
    for (i = 0; i < WS_MAX_CONNS; i++)
    {
        WsConn* w = &g_ws[i];
        if (!w->used) continue;
        if ((!w->open && now - w->acceptMs > WS_HANDSHAKE_MS) || now - w->lastRxMs > WS_IDLE_MS)
        {
            WsClose(w);
        }
    }
}

/* Adds the listener and every connection to the select() sets. */
static void WsFdSets(fd_set* rd, fd_set* wr, ShSock* maxFd)
{
    int i;
    if (g_wsListen == SH_INVALID_SOCK)
    {
        return;
    }
    FD_SET(g_wsListen, rd);
    if (g_wsListen > *maxFd) *maxFd = g_wsListen;
    for (i = 0; i < WS_MAX_CONNS; i++)
    {
        if (!g_ws[i].used) continue;
        FD_SET(g_ws[i].fd, rd);
        if (g_ws[i].txLen > 0) FD_SET(g_ws[i].fd, wr);
        if (g_ws[i].fd > *maxFd) *maxFd = g_ws[i].fd;
    }
}

static void WsService(fd_set* rd, fd_set* wr, unsigned int now)
{
    int i;
    if (g_wsListen == SH_INVALID_SOCK)
    {
        return;
    }
    if (FD_ISSET(g_wsListen, rd))
    {
        WsAccept(now);
    }
    for (i = 0; i < WS_MAX_CONNS; i++)
    {
        WsConn* w = &g_ws[i];
        if (!w->used) continue;
        if (FD_ISSET(w->fd, rd))
        {
            WsRead(w, now);
        }
        if (w->used && w->txLen > 0 && FD_ISSET(w->fd, wr) && !WsFlush(w))
        {
            WsClose(w);
        }
    }
}
#endif /* !SH_MASTER_EMBED */

#ifndef SH_MASTER_EMBED
/* ------------------------------------------------------------------ */
/* Admin console                                                       */
/* ------------------------------------------------------------------ */

/* Commands come in as text datagrams on 127.0.0.1:g_adminPort and the answer
 * goes back the same way. Loopback only, so whoever can send one is already
 * logged in to this machine. `sh_master --console` (or `--cmd "..."`) is the
 * client end, so a server running under systemd with no terminal can still
 * be told to kick someone or tidy the marker store.
 *
 * Bans are kept in bans.txt beside the marker store, one per line:
 *   ip|uid <value> <until unix, 0 = forever> <name or note>  */

#define ADM_REPLY_MAX 60000
#define ADM_BAN_MAX   512

typedef struct
{
    int     isIp;
    shn_u32 value;     /* IPv4 in network order, or a persistent player id */
    long    until;     /* unix seconds; 0 = never lifts */
    char    note[SHNET_NAME_MAX];
} AdmBan;

static ShSock          g_adminSock = SH_INVALID_SOCK;
static unsigned short  g_adminPort = SHNET_DEFAULT_PORT + 3;
static AdmBan          g_bans[ADM_BAN_MAX];
static int             g_banCount;
static char            g_banPath[600];

static int AdmStrCaseCmp(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : (unsigned char)*a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : (unsigned char)*b;
        if (ca != cb || !ca) return ca - cb;
    }
}

static char g_admOut[ADM_REPLY_MAX];
static int  g_admLen;

static void AdmPrintf(const char* fmt, ...)
{
    va_list ap;
    int     n;
    if (g_admLen >= ADM_REPLY_MAX - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(g_admOut + g_admLen, (size_t)(ADM_REPLY_MAX - g_admLen), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    g_admLen += n;
    if (g_admLen > ADM_REPLY_MAX - 1) g_admLen = ADM_REPLY_MAX - 1;
}

static void AdmIpStr(shn_u32 ipNet, char* out, int cap)
{
    unsigned int ip = ntohl(ipNet);
    snprintf(out, (size_t)cap, "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

/* Beside the marker store, so one -d moves both. */
static void AdmBanPathInit(void)
{
    const char* slash = strrchr(g_dbPath, '/');
#if defined(_WIN32)
    const char* bs = strrchr(g_dbPath, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    if (slash && g_dbPath[0])
        snprintf(g_banPath, sizeof(g_banPath), "%.*sbans.txt", (int)(slash - g_dbPath + 1), g_dbPath);
    else
        snprintf(g_banPath, sizeof(g_banPath), "bans.txt");
}

static void AdmSaveBans(void)
{
    FILE* f = fopen(g_banPath, "w");
    int   i;
    if (!f)
    {
        SrvLog("could not write %s", g_banPath);
        return;
    }
    fprintf(f, "# sh_master bans: ip|uid value until(unix, 0 = forever) note\n");
    for (i = 0; i < g_banCount; i++)
    {
        char v[32];
        if (g_bans[i].isIp) AdmIpStr(g_bans[i].value, v, (int)sizeof(v));
        else                snprintf(v, sizeof(v), "%u", g_bans[i].value);
        fprintf(f, "%s %s %ld %s\n", g_bans[i].isIp ? "ip" : "uid", v, g_bans[i].until,
                g_bans[i].note[0] ? g_bans[i].note : "-");
    }
    fclose(f);
}

static void AdmLoadBans(void)
{
    FILE* f;
    char  line[256];
    AdmBanPathInit();
    f = fopen(g_banPath, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && g_banCount < ADM_BAN_MAX)
    {
        char  kind[8], val[64], note[64];
        long  until = 0;
        AdmBan b;
        note[0] = '\0';
        if (line[0] == '#' || sscanf(line, "%7s %63s %ld %63[^\r\n]", kind, val, &until, note) < 3) continue;
        memset(&b, 0, sizeof(b));
        b.isIp  = !strcmp(kind, "ip");
        b.value = b.isIp ? (shn_u32)inet_addr(val) : (shn_u32)strtoul(val, NULL, 10);
        b.until = until;
        snprintf(b.note, sizeof(b.note), "%s", strcmp(note, "-") ? note : "");
        g_bans[g_banCount++] = b;
    }
    fclose(f);
    if (g_banCount) SrvLog("loaded %d bans from %s", g_banCount, g_banPath);
}

/* 1 when an address or persistent id is banned now; expired bans are dropped. */
static int AdmIsBanned(shn_u32 ipNet, shn_u32 uid)
{
    long t = (long)time(NULL);
    int  i, hit = 0, dropped = 0;
    for (i = 0; i < g_banCount; i++)
    {
        if (g_bans[i].until && g_bans[i].until <= t)
        {
            g_bans[i--] = g_bans[--g_banCount];
            dropped = 1;
            continue;
        }
        if (g_bans[i].isIp ? (g_bans[i].value == ipNet) : (uid && g_bans[i].value == uid)) hit = 1;
    }
    if (dropped) AdmSaveBans();
    return hit;
}

static void AdmAddBan(int isIp, shn_u32 value, long minutes, const char* note)
{
    int i;
    for (i = 0; i < g_banCount; i++)
    {
        if (g_bans[i].isIp == isIp && g_bans[i].value == value) break;
    }
    if (i == g_banCount)
    {
        if (g_banCount >= ADM_BAN_MAX) return;
        g_banCount++;
    }
    g_bans[i].isIp  = isIp;
    g_bans[i].value = value;
    g_bans[i].until = minutes > 0 ? (long)time(NULL) + minutes * 60 : 0;
    snprintf(g_bans[i].note, sizeof(g_bans[i].note), "%s", note ? note : "");
}

/* A player by id number or by name (case-insensitive). NULL with a reason in
 * the reply when there is none, or the name is not unique. */
static Client* AdmFindPlayer(const char* who)
{
    char*   end;
    unsigned long id = strtoul(who, &end, 10);
    Client* hit = NULL;
    int     i, n = 0;
    if (*who && !*end)
    {
        for (i = 0; i < SRV_MAX_CLIENTS; i++)
            if (g_clients[i].used && g_clients[i].playerId == (shn_u32)id) return &g_clients[i];
    }
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && !AdmStrCaseCmp(g_clients[i].name, who))
        {
            hit = &g_clients[i];
            n++;
        }
    }
    if (n == 1) return hit;
    if (n > 1) AdmPrintf("more than one player is called \"%s\"; use their id (see players)\n", who);
    else       AdmPrintf("no player \"%s\" (see players)\n", who);
    return NULL;
}

/* Tells the player why, then frees the slot as a goodbye would. The client
 * stays off this server until its player chooses to reconnect. */
static void AdmRemove(Client* c, const char* why, const char* verb)
{
    char text[SHNET_EVENT_MAX];
    SrvSendReject(&c->addr, SHNET_REJ_BANNED, why);
    snprintf(text, sizeof(text), "%s was %s", c->name, verb);
    SrvLog("- %s %s by the admin (id %u)", c->name, verb, c->playerId);
    SrvRoomLeave(c, 1);
    c->used = 0;
    SrvBroadcastEvent(SHNET_EV_LEAVE, c->playerId, text, NULL);
}

static void AdmCmdPlayers(void)
{
    unsigned int now = SrvMillis();
    int          i, n = 0;
    AdmPrintf("%-6s %-24s %-24s %4s %6s %5s %-10s %s\n", "id", "name", "address", "map", "room", "ping", "uid", "build");
    for (i = 0; i < SRV_MAX_CLIENTS; i++)
    {
        const Client* c = &g_clients[i];
        char          a[64];
        if (!c->used) continue;
        SrvAddrStr(&c->addr, a, (int)sizeof(a));
        AdmPrintf("%-6u %-24s %-24s %4d %6u %5d %-10u %s  (online %us)\n", c->playerId, c->name, a, c->mapIdx,
                  (unsigned)c->room, c->pingMs, c->uid, c->build, (now - c->joinMs) / 1000u);
        n++;
    }
    AdmPrintf("%d player(s)\n", n);
}

/* Marker owners match by persistent id (a number) or by the name they placed
 * it under (case-insensitive). */
static int AdmMemoOwnedBy(const Memo* m, const char* who)
{
    char*         end;
    unsigned long id = strtoul(who, &end, 10);
    if (*who && !*end && m->ownerId == (shn_u32)id) return 1;
    return !AdmStrCaseCmp(m->ownerName, who);
}

static void AdmCmdMemos(const char* who)
{
    int  i, shown = 0, total = 0;
    long t = (long)time(NULL);
    if (!who || !*who)
    {
        /* Who has placed the most, so a spammer stands out. */
        typedef struct { shn_u32 id; char name[SHNET_NAME_MAX]; int n; } Tally;
        static Tally tally[SRV_MAX_MEMOS > 4096 ? 4096 : SRV_MAX_MEMOS];
        int nt = 0, k;
        for (i = 0; i < g_memoCount; i++)
        {
            for (k = 0; k < nt && tally[k].id != g_memos[i].ownerId; k++) {}
            if (k == nt)
            {
                if (nt >= (int)(sizeof(tally) / sizeof(tally[0]))) continue;
                tally[nt].id = g_memos[i].ownerId;
                snprintf(tally[nt].name, sizeof(tally[nt].name), "%s", g_memos[i].ownerName);
                tally[nt].n = 0;
                nt++;
            }
            tally[k].n++;
        }
        for (k = 0; k < nt && k < 400; k++)
        {
            AdmPrintf("%6d  %-24s uid %u\n", tally[k].n, tally[k].name, tally[k].id);
        }
        AdmPrintf("%d markers from %d players\n", g_memoCount, nt);
        return;
    }
    AdmPrintf("%-8s %4s %-6s %7s %s\n", "id", "map", "kind", "rating", "age");
    for (i = 0; i < g_memoCount; i++)
    {
        const Memo* m = &g_memos[i];
        if (!AdmMemoOwnedBy(m, who)) continue;
        total++;
        if (shown < 200)
        {
            long days = m->placedUnix ? (t - m->placedUnix) / 86400 : -1;
            AdmPrintf("%-8u %4d %-6s %7d %ld days\n", m->memoId, m->mapIdx,
                      (m->kind & 0x0F) == SHNET_MARK_DEATH ? "death" : "memo", (int)m->rating, days);
            shown++;
        }
    }
    AdmPrintf("%d marker(s) by \"%s\"%s\n", total, who, total > shown ? " (first 200 shown)" : "");
}

/* kind: 0 older than `days`, 1 by owner, 2 by marker id, 3 death markers. */
static void AdmPrune(int kind, const char* arg, long days)
{
    long    cutoff = (long)time(NULL) - days * 86400;
    shn_u32 id     = (shn_u32)strtoul(arg ? arg : "0", NULL, 10);
    int     i, removed = 0;
    for (i = 0; i < g_memoCount; i++)
    {
        const Memo* m = &g_memos[i];
        int gone = (kind == 0) ? (m->placedUnix && m->placedUnix < cutoff)
                 : (kind == 1) ? AdmMemoOwnedBy(m, arg)
                 : (kind == 2) ? (m->memoId == id)
                 : ((m->kind & 0x0F) == SHNET_MARK_DEATH && (days <= 0 || (m->placedUnix && m->placedUnix < cutoff)));
        if (gone)
        {
            g_memos[i--] = g_memos[--g_memoCount];
            removed++;
        }
    }
    if (removed)
    {
        g_dbDirty = 1;
        SrvSaveDb();
    }
    SrvLog("admin pruned %d marker(s)", removed);
    AdmPrintf("removed %d marker(s); %d left. Players see the change on their next map change.\n",
              removed, g_memoCount);
}

static void AdmHelp(void)
{
    AdmPrintf(
        "players                      who is on, with ids, addresses and rooms\n"
        "kick <id|name> [reason]      remove a player (they can reconnect)\n"
        "ban <id|name|ip> [minutes] [reason]\n"
        "                             ban by address and player id; no minutes = forever\n"
        "unban <ip|uid>               lift a ban\n"
        "bans                         list bans\n"
        "memos [name|uid]             marker counts by player, or one player's markers\n"
        "prune old <days>             delete markers older than <days>\n"
        "prune user <name|uid>        delete every marker by a player\n"
        "prune id <marker id>         delete one marker\n"
        "prune deaths [days]          delete death markers (all, or older than <days>)\n"
        "say <text>                   message to everyone online\n"
        "save                         write the marker store now\n");
}

static void AdmRun(char* line)
{
    char* cmd;
    char* rest;
    char* p = line;

    g_admLen    = 0;
    g_admOut[0] = '\0';
    while (*p == ' ' || *p == '\t') p++;
    {
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) p[--n] = '\0';
    }
    cmd  = p;
    rest = strchr(p, ' ');
    if (rest)
    {
        *rest++ = '\0';
        while (*rest == ' ') rest++;
    }
    else
    {
        rest = p + strlen(p);
    }
    if (!*cmd) { AdmHelp(); return; }
    SrvLog("admin: %s %s", cmd, rest);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) AdmHelp();
    else if (!strcmp(cmd, "players") || !strcmp(cmd, "list") || !strcmp(cmd, "who")) AdmCmdPlayers();
    else if (!strcmp(cmd, "kick"))
    {
        char*   why = strchr(rest, ' ');
        Client* c;
        char    text[SHNET_REJECT_MAX];
        if (why) { *why++ = '\0'; while (*why == ' ') why++; }
        if (!*rest) { AdmPrintf("kick <id|name> [reason]\n"); return; }
        c = AdmFindPlayer(rest);
        if (!c) return;
        snprintf(text, sizeof(text), "Removed by the server admin%s%s", (why && *why) ? ": " : "", why ? why : "");
        AdmPrintf("kicked %s (id %u)\n", c->name, c->playerId);
        AdmRemove(c, text, "removed");
    }
    else if (!strcmp(cmd, "ban"))
    {
        char*   tok = rest;
        char*   more = strchr(tok, ' ');
        long    minutes = 0;
        char*   why = NULL;
        Client* c = NULL;
        char    text[SHNET_REJECT_MAX];
        if (more) { *more++ = '\0'; while (*more == ' ') more++; }
        if (!*tok) { AdmPrintf("ban <id|name|ip> [minutes] [reason]\n"); return; }
        if (more && *more >= '0' && *more <= '9')
        {
            minutes = strtol(more, &why, 10);
            while (why && *why == ' ') why++;
        }
        else
        {
            why = more;
        }
        snprintf(text, sizeof(text), "Banned from this server%s%s", (why && *why) ? ": " : "", why ? why : "");
        if (strchr(tok, '.') && inet_addr(tok) != INADDR_NONE)
        {
            shn_u32 ip = (shn_u32)inet_addr(tok);
            int     i;
            AdmAddBan(1, ip, minutes, why);
            for (i = 0; i < SRV_MAX_CLIENTS; i++)
            {
                if (g_clients[i].used && g_clients[i].addr.sin_addr.s_addr == ip)
                {
                    if (g_clients[i].uid) AdmAddBan(0, g_clients[i].uid, minutes, g_clients[i].name);
                    AdmRemove(&g_clients[i], text, "banned");
                }
            }
            AdmPrintf("banned %s%s\n", tok, minutes ? "" : " for good");
        }
        else
        {
            c = AdmFindPlayer(tok);
            if (!c) return;
            AdmAddBan(1, c->addr.sin_addr.s_addr, minutes, c->name);
            if (c->uid) AdmAddBan(0, c->uid, minutes, c->name);
            AdmPrintf("banned %s (id %u)%s\n", c->name, c->playerId, minutes ? "" : " for good");
            AdmRemove(c, text, "banned");
        }
        if (minutes) AdmPrintf("lifts in %ld minute(s)\n", minutes);
        AdmSaveBans();
    }
    else if (!strcmp(cmd, "unban"))
    {
        int     i, n = 0, isIp = strchr(rest, '.') != NULL;
        shn_u32 v = isIp ? (shn_u32)inet_addr(rest) : (shn_u32)strtoul(rest, NULL, 10);
        for (i = 0; i < g_banCount; i++)
        {
            if (g_bans[i].isIp == isIp && g_bans[i].value == v)
            {
                g_bans[i--] = g_bans[--g_banCount];
                n++;
            }
        }
        AdmSaveBans();
        AdmPrintf(n ? "unbanned %s\n" : "no ban on %s (see bans)\n", rest);
    }
    else if (!strcmp(cmd, "bans"))
    {
        int  i;
        long t = (long)time(NULL);
        AdmIsBanned(0, 0); /* drops expired ones */
        for (i = 0; i < g_banCount; i++)
        {
            char v[32];
            if (g_bans[i].isIp) AdmIpStr(g_bans[i].value, v, (int)sizeof(v));
            else                snprintf(v, sizeof(v), "%u", g_bans[i].value);
            if (g_bans[i].until)
                AdmPrintf("%-3s %-16s %ld min left  %s\n", g_bans[i].isIp ? "ip" : "uid", v,
                          (g_bans[i].until - t + 59) / 60, g_bans[i].note);
            else
                AdmPrintf("%-3s %-16s forever       %s\n", g_bans[i].isIp ? "ip" : "uid", v, g_bans[i].note);
        }
        AdmPrintf("%d ban(s)\n", g_banCount);
    }
    else if (!strcmp(cmd, "memos")) AdmCmdMemos(rest);
    else if (!strcmp(cmd, "prune"))
    {
        char* arg = strchr(rest, ' ');
        if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }
        if (!strcmp(rest, "old") && arg && atol(arg) > 0)        AdmPrune(0, NULL, atol(arg));
        else if (!strcmp(rest, "user") && arg && *arg)          AdmPrune(1, arg, 0);
        else if (!strcmp(rest, "id") && arg && *arg)            AdmPrune(2, arg, 0);
        else if (!strcmp(rest, "deaths"))                       AdmPrune(3, NULL, arg ? atol(arg) : 0);
        else AdmPrintf("prune old <days> | prune user <name|uid> | prune id <marker id> | prune deaths [days]\n");
    }
    else if (!strcmp(cmd, "say"))
    {
        if (!*rest) { AdmPrintf("say <text>\n"); return; }
        SrvBroadcastEvent(SHNET_EV_SERVER, 0, rest, NULL);
        AdmPrintf("sent\n");
    }
    else if (!strcmp(cmd, "save"))
    {
        SrvSaveDb();
        AdmSaveBans();
        AdmPrintf("saved %d markers to %s\n", g_memoCount, g_dbPath);
    }
    else AdmPrintf("unknown command \"%s\" (try help)\n", cmd);
}

static int AdmListen(void)
{
    struct sockaddr_in a;
    g_adminSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_adminSock == SH_INVALID_SOCK) return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = htons(g_adminPort);
    if (bind(g_adminSock, (struct sockaddr*)&a, sizeof(a)) != 0)
    {
        SH_CLOSESOCK(g_adminSock);
        g_adminSock = SH_INVALID_SOCK;
        return 0;
    }
    WsSetNonBlocking(g_adminSock);
    return 1;
}

static void AdmService(void)
{
    for (;;)
    {
        char               line[1024];
        struct sockaddr_in from;
        ShSockLen          flen = (ShSockLen)sizeof(from);
        int n = (int)recvfrom(g_adminSock, line, sizeof(line) - 1, 0, (struct sockaddr*)&from, &flen);
        if (n <= 0) return;
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        line[n] = '\0';
        AdmRun(line);
        if (g_admLen == 0) AdmPrintf("ok\n");
        sendto(g_adminSock, g_admOut, (size_t)g_admLen, 0, (struct sockaddr*)&from, flen);
    }
}

/* The client end: send one command, print the answer. 0 when the server did
 * not answer. */
static int AdmSendOne(ShSock s, const struct sockaddr_in* to, const char* cmd)
{
    static char    buf[ADM_REPLY_MAX + 1];
    fd_set         rd;
    struct timeval tv;
    int            n;
    sendto(s, cmd, strlen(cmd), 0, (const struct sockaddr*)to, (ShSockLen)sizeof(*to));
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    if (select((int)s + 1, &rd, NULL, NULL, &tv) <= 0)
    {
        printf("no answer from the server on 127.0.0.1:%u (is it running? --admin PORT if it uses another)\n",
               (unsigned)g_adminPort);
        return 0;
    }
    n = (int)recv(s, buf, ADM_REPLY_MAX, 0);
    if (n > 0)
    {
        buf[n] = '\0';
        fputs(buf, stdout);
        fflush(stdout);
    }
    return 1;
}

static int AdmClient(const char* oneCmd)
{
    struct sockaddr_in to;
    ShSock             s;
    char               line[1024];
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port        = htons(g_adminPort);
    if (oneCmd)
    {
        return AdmSendOne(s, &to, oneCmd) ? 0 : 1;
    }
    printf("Silent Hill Online server console. Type help for commands, quit to leave.\n");
    for (;;)
    {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, "quit") || !strcmp(line, "exit")) break;
        if (!line[0]) continue;
        AdmSendOne(s, &to, line);
    }
    return 0;
}
#endif /* !SH_MASTER_EMBED */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int ShMaster_Serve(void);

#ifndef SH_MASTER_EMBED
static void SrvUsage(const char* argv0)
{
    printf("Silent Hill Online master server (protocol v%d)\n\n", SHNET_PROTO_VER);
    printf("  %s [options]\n\n", argv0);
    printf("  -p PORT      UDP port to listen on (default %d)\n", SHNET_DEFAULT_PORT);
    printf("  --ws PORT    also take browser players over WebSocket on TCP PORT\n");
    printf("  --admin PORT admin console port on 127.0.0.1 (default %d, 0 = off)\n", SHNET_DEFAULT_PORT + 3);
    printf("  --console    talk to the running server's admin console\n");
    printf("  --cmd TEXT   run one admin command on the running server (try help)\n");
    printf("               (behind a TLS proxy: browsers need wss://)\n");
    printf("  -n NAME      server name shown to players\n");
    printf("  -m TEXT      message of the day\n");
    printf("  -w WORD      require this password\n");
    printf("  -b ID        only admit clients whose build id is ID (repeatable)\n");
    printf("  --builds F   read allowed build ids from file F, one per line\n");
    printf("               (a client's id is logged on every join: \"build ...\")\n");
    printf("  -d FILE      marker store path (default memos.db)\n");
    printf("  --max N      maximum simultaneous players (default 512, up to 1024)\n");
    printf("  -i N         maximum players from one IP address (default %d)\n", SRV_MAX_PER_IP_DEFAULT);
    printf("  --no-ghosts  do not fan out positions\n");
    printf("  --no-memos   do not accept or serve messages\n");
    printf("  --no-deaths  do not accept or serve death markers\n");
    printf("  -v           log every query\n");
    printf("  -h           this text\n");
}

int main(int argc, char** argv)
{
    int         i;
    int         console = 0;
    const char* oneCmd  = NULL;

    for (i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc)
        {
            g_port = (unsigned short)atoi(argv[++i]);
        }
        else if (!strcmp(a, "--admin") && i + 1 < argc)
        {
            g_adminPort = (unsigned short)atoi(argv[++i]);
        }
        else if (!strcmp(a, "--console"))
        {
            console = 1;
        }
        else if (!strcmp(a, "--cmd") && i + 1 < argc)
        {
            oneCmd = argv[++i];
        }
        else if (!strcmp(a, "--ws") && i + 1 < argc)
        {
            g_wsPort = (unsigned short)atoi(argv[++i]);
        }
        else if (!strcmp(a, "-n") && i + 1 < argc)
        {
            snprintf(g_serverName, sizeof(g_serverName), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-m") && i + 1 < argc)
        {
            snprintf(g_motd, sizeof(g_motd), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-w") && i + 1 < argc)
        {
            snprintf(g_password, sizeof(g_password), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-i") && i + 1 < argc)
        {
            g_maxPerIp = atoi(argv[++i]);
            if (g_maxPerIp < 1)               g_maxPerIp = 1;
            if (g_maxPerIp > SRV_MAX_CLIENTS) g_maxPerIp = SRV_MAX_CLIENTS;
        }
        else if (!strcmp(a, "-b") && i + 1 < argc)
        {
            SrvAllowBuild(argv[++i]);
        }
        else if (!strcmp(a, "--builds") && i + 1 < argc)
        {
            SrvLoadBuildsFile(argv[++i]);
        }
        else if (!strcmp(a, "-d") && i + 1 < argc)
        {
            snprintf(g_dbPath, sizeof(g_dbPath), "%s", argv[++i]);
        }
        else if (!strcmp(a, "--max") && i + 1 < argc)
        {
            g_maxClients = atoi(argv[++i]);
            if (g_maxClients < 1)              g_maxClients = 1;
            if (g_maxClients > SRV_MAX_CLIENTS) g_maxClients = SRV_MAX_CLIENTS;
        }
        else if (!strcmp(a, "--no-ghosts")) g_featureFlags &= ~SHNET_SF_GHOSTS;
        else if (!strcmp(a, "--no-memos"))  g_featureFlags &= ~SHNET_SF_MEMOS;
        else if (!strcmp(a, "--no-deaths")) g_featureFlags &= ~SHNET_SF_DEATHS;
        else if (!strcmp(a, "-v"))          g_verbose = 1;
        else
        {
            SrvUsage(argv[0]);
            return (strcmp(a, "-h") && strcmp(a, "--help")) ? 1 : 0;
        }
    }

    if (console || oneCmd)
    {
        return AdmClient(oneCmd);
    }
    SrvSanitize(g_serverName, SHNET_SERVER_MAX);
    return ShMaster_Serve();
}
#endif /* !SH_MASTER_EMBED */

/* The server proper: everything is configured through the g_* globals (by main's
 * argument parsing, or by ShMaster_SetConfig when embedded in the client); this
 * binds the socket and serves until g_running clears. */
int ShMaster_Serve(void)
{
    struct sockaddr_in bindAddr;
    unsigned int       lastExpire = 0;

    /* Fresh every start: a restart invalidates outstanding cookies, which only
     * costs a client one extra CHALLENGE round trip. */
    SrvRandomBytes(g_cookieKey, (int)sizeof(g_cookieKey));

    g_memos = (Memo*)calloc(SRV_MAX_MEMOS, sizeof(Memo));
    if (!g_memos)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

#if defined(_WIN32)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#endif

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == SH_INVALID_SOCK)
    {
        fprintf(stderr, "socket() failed\n");
        return 1;
    }

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(g_port);
    if (bind(g_sock, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) != 0)
    {
        fprintf(stderr, "could not bind UDP port %u (already in use?)\n", (unsigned)g_port);
        return 1;
    }

    /* Non-blocking: the receive loop below drains until the socket is empty,
     * and on a blocking socket that last read parks the whole server between
     * datagrams, which stops the snapshot fan-out running at all. */
#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
    }
#else
    {
        int fl = fcntl(g_sock, F_GETFL, 0);
        fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);
    }
#endif

#if defined(_WIN32)
    {
        /* An ICMP port-unreachable from a client that just quit otherwise
         * latches recvfrom into a permanent WSAECONNRESET. */
        BOOL  b   = FALSE;
        DWORD ret = 0;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        WSAIoctl(g_sock, SIO_UDP_CONNRESET, &b, sizeof(b), NULL, 0, &ret, NULL, NULL);
    }
#endif

#ifndef SH_MASTER_EMBED
    if (g_wsPort && !WsListen(g_wsPort))
    {
        fprintf(stderr, "could not listen on TCP port %u for WebSocket clients\n", (unsigned)g_wsPort);
        return 1;
    }
    {
        int i;
        for (i = 0; i < WS_MAX_CONNS; i++) g_ws[i].fd = SH_INVALID_SOCK;
    }
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* a browser tab closing mid-send */
#endif
    signal(SIGINT, SrvOnSignal);
#ifdef SIGTERM
    signal(SIGTERM, SrvOnSignal);
#endif
#endif

    SrvLoadDb();
    g_lastSaveMs = SrvMillis();
#ifndef SH_MASTER_EMBED
    AdmLoadBans();
    if (g_adminPort && !AdmListen())
    {
        SrvLog("admin console unavailable: 127.0.0.1:%u is in use", (unsigned)g_adminPort);
    }
#endif

    SrvLog("\"%s\" listening on UDP %u  (max %d players, %d per address%s%s%s%s)",
           g_serverName, (unsigned)g_port, g_maxClients, g_maxPerIp,
           g_password[0] ? ", password" : "",
           (g_featureFlags & SHNET_SF_GHOSTS) ? "" : ", no ghosts",
           (g_featureFlags & SHNET_SF_MEMOS)  ? "" : ", no memos",
           (g_featureFlags & SHNET_SF_DEATHS) ? "" : ", no deaths");
#ifndef SH_MASTER_EMBED
    if (g_wsListen != SH_INVALID_SOCK)
    {
        SrvLog("browser players: WebSocket on TCP %u", (unsigned)g_wsPort);
    }
#endif

    while (g_running)
    {
        fd_set          rd, wr;
        struct timeval  tv;
        unsigned int    now;
        ShSock          maxFd = g_sock;
        int             ready;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(g_sock, &rd);
#ifndef SH_MASTER_EMBED
        WsFdSets(&rd, &wr, &maxFd);
        if (g_adminSock != SH_INVALID_SOCK)
        {
            FD_SET(g_adminSock, &rd);
            if (g_adminSock > maxFd) maxFd = g_adminSock;
        }
#endif
        tv.tv_sec  = 0;
        tv.tv_usec = 20000;

        ready = select((int)maxFd + 1, &rd, &wr, NULL, &tv) > 0;
#ifndef SH_MASTER_EMBED
        if (ready)
        {
            WsService(&rd, &wr, SrvMillis());
            if (g_adminSock != SH_INVALID_SOCK && FD_ISSET(g_adminSock, &rd))
            {
                AdmService();
            }
        }
#endif
        if (ready && FD_ISSET(g_sock, &rd))
        {
            int budget;
            for (budget = 0; budget < SRV_RX_PER_PASS; budget++)
            {
                shn_u8             buf[SHNET_MTU];
                struct sockaddr_in from;
                ShSockLen          flen = (ShSockLen)sizeof(from);
                int                n;

                n = (int)recvfrom(g_sock, (char*)buf, sizeof(buf), 0,
                                  (struct sockaddr*)&from, &flen);
                if (n <= 0)
                {
                    break;
                }
                memset(from.sin_zero, 0, sizeof(from.sin_zero));
                SrvDispatch(&from, buf, n);
            }
        }

        now = SrvMillis();
        SrvSendSnapshots(now);

        if (now - lastExpire >= 1000u)
        {
            lastExpire = now;
            SrvExpire(now);
#ifndef SH_MASTER_EMBED
            WsExpire(now);
#endif
        }
        if (g_dbDirty && (now - g_lastSaveMs) >= SRV_SAVE_GAP_MS)
        {
            g_lastSaveMs = now;
            SrvSaveDb();
        }
    }

    SrvLog("shutting down");
    SrvSaveDb();
    SH_CLOSESOCK(g_sock);
#if defined(_WIN32)
    WSACleanup();
#endif
    free(g_memos);
    g_sock = SH_INVALID_SOCK;
    return 0;
}

#ifdef SH_MASTER_EMBED
/* Run inside the game as a co-op listen host. The relay's living-world extras
 * (ghosts, memos) stay idle because no one asks for them, and the empty db path
 * means it never touches the disk. ShMaster_Serve blocks on its own thread until
 * ShMaster_RequestStop clears g_running. */
void ShMaster_SetConfig(unsigned short port, int maxPlayers, const char* name)
{
    g_port = port ? port : SHNET_DEFAULT_PORT;
    if (maxPlayers > 0)
    {
        g_maxClients = maxPlayers;
        if (g_maxClients > SRV_MAX_CLIENTS) g_maxClients = SRV_MAX_CLIENTS;
    }
    if (name && name[0]) snprintf(g_serverName, sizeof(g_serverName), "%s", name);
    g_dbPath[0] = '\0';   /* no marker store on disk when embedded */
    /* Advertise only what this relay serves, so clients do not wait on ghost
     * snapshots or marker lists that will never come. */
    g_featureFlags &= ~(SHNET_SF_MEMOS | SHNET_SF_GHOSTS);
    g_running   = 1;
}

void ShMaster_RequestStop(void) { g_running = 0; }

int ShMaster_Running(void) { return g_running && g_sock != SH_INVALID_SOCK; }
#endif
