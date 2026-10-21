/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_proto.h - Silent Hill Online wire protocol, version 1.
 *
 * THE CONTRACT between the game client (pc_port/src/net) and the master server
 * (online_server/sh_master.c). Both compile this file verbatim; it is the one
 * place either side may learn the byte layout, so it deliberately includes
 * nothing at all — not even <stdint.h>, which cannot be pulled into a
 * translation unit that also sees the decomp's own u8/u32 typedefs.
 *
 * Transport is UDP. Every field is written through the ShnPut / ShnGet helpers
 * below rather than by casting a struct over the buffer: the client is built by
 * MinGW for x86-64 and the server may be built by anything, and a struct laid
 * over a datagram is a promise about padding and alignment that neither
 * compiler made.
 *
 * Byte order is LITTLE-ENDIAN on the wire, chosen because every machine that
 * will ever run either end is little-endian and the helpers make the choice
 * free. The helpers are endian-agnostic, so a big-endian build (the port has
 * PS3/360/N64 branches) interoperates without a #ifdef.
 *
 * Datagrams are capped at SHNET_MTU so nothing this protocol emits can be
 * fragmented by a home router; anything larger (a full roster, a busy map's
 * memo list) is paged across several datagrams instead.
 *
 * RELIABILITY: there is none, by design. STATE and SNAPSHOT are pure
 * fire-and-forget — a dropped position update is corrected 100ms later. The
 * handful of messages that DO matter (HELLO, MEMO_PLACE, MEMO_QUERY,
 * ROSTER_REQ) are re-sent by the client on a timer until the matching reply
 * arrives, which is the whole reliability layer and is enough for a protocol
 * with no ordering requirements.
 */
#ifndef SH_NET_PROTO_H
#define SH_NET_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width aliases. Spelled out rather than taken from <stdint.h> so this
 * header stays includable from a decomp TU (whose own u8/u16/u32 would collide
 * with a system header's). The static assert below is the only guarantee
 * needed: every target is a 8/16/32 machine for these three widths. */
typedef unsigned char  shn_u8;
typedef unsigned short shn_u16;
typedef unsigned int   shn_u32;
typedef signed char    shn_s8;
typedef short          shn_s16;
typedef int            shn_s32;

/* GCC's -Wunused-function fires on a static helper no TU happens to call, and
 * every TU that includes this header uses a different subset. `static inline`
 * with the unused attribute keeps internal linkage without the noise. */
#if defined(__GNUC__)
#define SHN_FN static __inline__ __attribute__((unused))
#else
#define SHN_FN static
#endif

typedef char shn_static_assert_widths[
    (sizeof(shn_u8) == 1 && sizeof(shn_u16) == 2 && sizeof(shn_u32) == 4) ? 1 : -1];

#define SHNET_PROTO_VER   1
#define SHNET_MAGIC0      'S'
#define SHNET_MAGIC1      'H'
#define SHNET_MAGIC2      'O'
#define SHNET_MAGIC3      '1'

/* Payload cap. 1200 keeps the IP datagram under the 1280-byte floor every
 * IPv6 path must carry and well under a 1500-byte Ethernet MTU with room for
 * a PPPoE or VPN header, so nothing here is ever fragmented. */
#define SHNET_MTU         1200
#define SHNET_HDR_SIZE    12
#define SHNET_MAX_PAYLOAD (SHNET_MTU - SHNET_HDR_SIZE)

#define SHNET_DEFAULT_PORT 27888

#define SHNET_NAME_MAX     24   /* including the NUL */
#define SHNET_BUILD_MAX    24
#define SHNET_SERVER_MAX   32
#define SHNET_MOTD_MAX     128
#define SHNET_REJECT_MAX   96
#define SHNET_EVENT_MAX    96

/* Upper bounds the server enforces on memo template indices. The tables live
 * in sh_net_memo.c and are append-only; it asserts it stays under these. */
#define SHNET_MEMO_PHRASE_MAX 64
#define SHNET_MEMO_WORD_MAX   256
#define SHNET_MEMO_NONE       0xFFFF /* second half of a memo left empty */

/* World coordinates any sane pose stays within (Q19.12, +-2048 m). */
#define SHNET_COORD_MAX    (2048 << 12)

/* ------------------------------------------------------------------ */
/* Message types                                                       */
/* ------------------------------------------------------------------ */
enum
{
    SHNET_MSG_HELLO      = 0x01, /* C->S  join / reconnect */
    SHNET_MSG_WELCOME    = 0x02, /* S->C  accepted, here is your session */
    SHNET_MSG_PING       = 0x03, /* C->S */
    SHNET_MSG_PONG       = 0x04, /* S->C */
    SHNET_MSG_BYE        = 0x05, /* C->S  clean disconnect */
    SHNET_MSG_REJECT     = 0x06, /* S->C  refused, with a reason */
    SHNET_MSG_CHALLENGE  = 0x07, /* S->C  echo this cookie in your HELLO (u32) */

    SHNET_MSG_STATE      = 0x10, /* C->S  this player's position, ~10Hz */
    SHNET_MSG_SNAPSHOT   = 0x11, /* S->C  everyone else on this map, ~10Hz */

    SHNET_MSG_ROSTER_REQ = 0x20, /* C->S  who is online (for the player list) */
    SHNET_MSG_ROSTER     = 0x21, /* S->C  one page of the roster */

    SHNET_MSG_MEMO_PLACE = 0x30, /* C->S  leave a message / death marker */
    SHNET_MSG_MEMO_QUERY = 0x31, /* C->S  what is on this map */
    SHNET_MSG_MEMO_LIST  = 0x32, /* S->C  one page of markers */
    SHNET_MSG_MEMO_RATE  = 0x33, /* C->S  +1 / -1 on someone's memo */
    SHNET_MSG_MEMO_ACK   = 0x34, /* S->C  your placement landed, here is its id */

    SHNET_MSG_CHAT_SAY   = 0x35, /* C->S  a line the player typed (scope + text) */
    SHNET_MSG_CHAT_MSG   = 0x36, /* S->C  a line to show (scope + sender + text) */

    SHNET_MSG_EVENT      = 0x40, /* S->C  a line for the toast feed */

    /* ---- Session messages ----
     * Peer to peer between the members of a Steam lobby, NOT to the master
     * server. They travel on the Steam relay (sh_net_steam.c) rather than the
     * UDP socket, but they carry the same header and go through the same
     * ShnPut/ShnGet helpers, so one parser covers both and a co-op message
     * added later is just another type in this range. */
    SHNET_MSG_S_HELLO    = 0x50, /* P2P  I am here, this is my name and map */
    SHNET_MSG_S_PING     = 0x51,
    SHNET_MSG_S_PONG     = 0x52,
    SHNET_MSG_S_BYE      = 0x53,
    SHNET_MSG_S_POS      = 0x54, /* P2P  my position/anim this tick (co-op presence) */
    SHNET_MSG_S_WORLD    = 0x55, /* P2P  host -> guests: boot into this map */
    SHNET_MSG_S_ITEM     = 0x56, /* P2P  I picked this up; everyone gets it too */
    SHNET_MSG_S_DAMAGE   = 0x57, /* P2P  I hit YOU: apply this damage to yourself (friendly fire) */
    SHNET_MSG_S_KICK     = 0x58, /* P2P  host -> member: you have been removed */
    SHNET_MSG_S_NPC_SNAP = 0x59, /* P2P  host -> all: the monsters it runs (sh_net_npc.c) */
    SHNET_MSG_S_NPC_HIT  = 0x5A, /* P2P  member -> host: I hit your monster */
    SHNET_MSG_S_NPC_ATK  = 0x5B, /* P2P  host -> member: your monster hit you */
    SHNET_MSG_S_WORLD_FL = 0x5C, /* P2P  host -> all: event flags + enemy states (the world's progress) */
    SHNET_MSG_S_FLAG_SET = 0x5D, /* P2P  member -> all: I took this pickup (event flag index) */
    SHNET_MSG_S_CTRL     = 0x5E, /* P2P  small game-control notes, first byte = SHNET_CTRL_* kind
                                  *      (pause, pause request, permissions, voice line, cutscene) */

    /* Co-op over a relay server (sh_master rooms). The transport the session layer
     * rides when it is NOT using Steam P2P: the host creates a room, guests join
     * by code or from a list, and the server forwards each member's traffic to
     * the rest of the room. A COOP_RELAY payload is itself one of the S_* messages
     * above, so the session's own wire format is reused unchanged inside it. */
    SHNET_MSG_ROOM_CREATE   = 0x60, /* C->S  open a room (u8 maxPlayers, name) */
    SHNET_MSG_ROOM_JOIN     = 0x61, /* C->S  join a room by code (u16 code) */
    SHNET_MSG_ROOM_LEAVE    = 0x62, /* C->S  leave the current room */
    SHNET_MSG_ROOM_LIST_REQ = 0x63, /* C->S  list open rooms */
    SHNET_MSG_ROOM_JOINED   = 0x64, /* S->C  you are in: your member id, code, host id, roster */
    SHNET_MSG_ROOM_ROSTER   = 0x65, /* S->C  the room roster changed */
    SHNET_MSG_ROOM_REJECT   = 0x66, /* S->C  create/join refused (reason) */
    SHNET_MSG_ROOM_LIST     = 0x67, /* S->C  one page of open rooms */
    SHNET_MSG_COOP_RELAY    = 0x68, /* C->S fromMe / S->C fromPeer: (u32 peerId, S_* payload) */
    SHNET_MSG_ROOM_KICK     = 0x69  /* C->S  room host removes a member (u32 playerId) */
};

/* Marker kinds. */
enum
{
    SHNET_MARK_MEMO  = 0, /* templated player message */
    SHNET_MARK_DEATH = 1, /* someone died here */
    SHNET_MARK_SAVE  = 2  /* someone saved here (a lit spot in the fog) */
};

/* STATE / SNAPSHOT player flag bits. */
enum
{
    SHNET_PF_ALIVE      = 1 << 0,
    SHNET_PF_FLASHLIGHT = 1 << 1,
    SHNET_PF_RUNNING    = 1 << 2,
    SHNET_PF_CUTSCENE   = 1 << 3, /* ghost hides: the player is not really "there" */
    SHNET_PF_AIMING     = 1 << 4,
    SHNET_PF_MENU       = 1 << 5,
    SHNET_PF_GCONSOLE   = 1 << 6, /* host only: guests may use the console and debug keys */
    SHNET_PF_FFIRE      = 1 << 7  /* accepts hits from other players (friendly fire is on) */
};

/* WELCOME feature bits — a server can host ghosts without memos, etc. */
enum
{
    SHNET_SF_GHOSTS = 1 << 0,
    SHNET_SF_MEMOS  = 1 << 1,
    SHNET_SF_DEATHS = 1 << 2,
    SHNET_SF_EVENTS = 1 << 3
};

/* REJECT reasons. */
enum
{
    SHNET_REJ_VERSION  = 1,
    SHNET_REJ_FULL     = 2,
    SHNET_REJ_PASSWORD = 3,
    SHNET_REJ_BANNED   = 4,
    SHNET_REJ_NAME     = 5,
    SHNET_REJ_BUILD    = 6  /* build id not on the server's allow-list */
};

/* Chat scope. Also the value the U key cycles through. */
enum
{
    SHNET_CHAT_GLOBAL = 0, /* everyone connected to the server */
    SHNET_CHAT_GAME   = 1  /* everyone on the same map (a "game", for now) */
};

#define SHNET_CHAT_MAX 160   /* max UTF-8 bytes of one chat line, incl. NUL */

/* EVENT kinds. */
enum
{
    SHNET_EV_JOIN   = 0,
    SHNET_EV_LEAVE  = 1,
    SHNET_EV_DEATH  = 2,
    SHNET_EV_SERVER = 3,
    SHNET_EV_MEMO   = 4
};

/* Entry sizes, so both ends page identically. */
#define SHNET_SNAP_ENTRY   24
#define SHNET_ROSTER_ENTRY 36
#define SHNET_MEMO_ENTRY   34

#define SHNET_SNAP_MAX   ((SHNET_MAX_PAYLOAD - 8) / SHNET_SNAP_ENTRY)
#define SHNET_ROSTER_MAX ((SHNET_MAX_PAYLOAD - 8) / SHNET_ROSTER_ENTRY)
#define SHNET_MEMO_MAX   ((SHNET_MAX_PAYLOAD - 8) / SHNET_MEMO_ENTRY)

/* ------------------------------------------------------------------ */
/* Serialization primitives                                            */
/* ------------------------------------------------------------------ */
/* Cursor-style: every helper advances *off. The caller checks the final
 * offset against the buffer size once, rather than at every field. Callers
 * MUST size the buffer for the worst case before writing. */

SHN_FN void ShnPutU8(shn_u8* b, int* off, shn_u8 v)
{
    b[(*off)++] = v;
}

SHN_FN void ShnPutU16(shn_u8* b, int* off, shn_u16 v)
{
    b[(*off)++] = (shn_u8)(v & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 8) & 0xFF);
}

SHN_FN void ShnPutU32(shn_u8* b, int* off, shn_u32 v)
{
    b[(*off)++] = (shn_u8)(v & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 8) & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 16) & 0xFF);
    b[(*off)++] = (shn_u8)((v >> 24) & 0xFF);
}

SHN_FN void ShnPutS32(shn_u8* b, int* off, shn_s32 v)
{
    ShnPutU32(b, off, (shn_u32)v);
}

SHN_FN void ShnPutS16(shn_u8* b, int* off, shn_s16 v)
{
    ShnPutU16(b, off, (shn_u16)v);
}

/* Fixed-width field, always exactly `cap` bytes on the wire, NUL padded and
 * NUL terminated. A short string does not shorten the datagram: fixed offsets
 * are what let a reader skip a field it does not care about. */
SHN_FN void ShnPutStr(shn_u8* b, int* off, const char* s, int cap)
{
    int i = 0;
    while (i < cap - 1 && s != 0 && s[i] != '\0')
    {
        b[*off + i] = (shn_u8)s[i];
        i++;
    }
    while (i < cap)
    {
        b[*off + i] = 0;
        i++;
    }
    *off += cap;
}

SHN_FN shn_u8 ShnGetU8(const shn_u8* b, int* off)
{
    return b[(*off)++];
}

SHN_FN shn_u16 ShnGetU16(const shn_u8* b, int* off)
{
    shn_u16 v = (shn_u16)(b[*off] | ((shn_u16)b[*off + 1] << 8));
    *off += 2;
    return v;
}

SHN_FN shn_u32 ShnGetU32(const shn_u8* b, int* off)
{
    shn_u32 v = (shn_u32)b[*off] | ((shn_u32)b[*off + 1] << 8) |
                ((shn_u32)b[*off + 2] << 16) | ((shn_u32)b[*off + 3] << 24);
    *off += 4;
    return v;
}

SHN_FN shn_s32 ShnGetS32(const shn_u8* b, int* off)
{
    return (shn_s32)ShnGetU32(b, off);
}

SHN_FN shn_s16 ShnGetS16(const shn_u8* b, int* off)
{
    return (shn_s16)ShnGetU16(b, off);
}

/* Reads `cap` wire bytes into a caller buffer of `outCap`, always NUL
 * terminating. The wire field is consumed in full even when the destination is
 * smaller, or every field after it would be read at the wrong offset. */
SHN_FN void ShnGetStr(const shn_u8* b, int* off, int cap, char* out, int outCap)
{
    int i;
    for (i = 0; i < cap; i++)
    {
        if (i < outCap - 1)
        {
            out[i] = (char)b[*off + i];
        }
    }
    if (outCap > 0)
    {
        out[(cap < outCap - 1) ? cap : outCap - 1] = '\0';
    }
    *off += cap;
}

/* ------------------------------------------------------------------ */
/* Header                                                              */
/* ------------------------------------------------------------------ */

SHN_FN void ShnPutHeader(shn_u8* b, shn_u8 type, shn_u16 payloadLen, shn_u32 session)
{
    int off = 0;
    ShnPutU8(b, &off, SHNET_MAGIC0);
    ShnPutU8(b, &off, SHNET_MAGIC1);
    ShnPutU8(b, &off, SHNET_MAGIC2);
    ShnPutU8(b, &off, SHNET_MAGIC3);
    ShnPutU8(b, &off, SHNET_PROTO_VER);
    ShnPutU8(b, &off, type);
    ShnPutU16(b, &off, payloadLen);
    ShnPutU32(b, &off, session);
}

/* Returns 1 and fills the out params when `b` is a well-formed datagram of
 * `len` bytes whose declared payload actually fits; 0 otherwise. A caller that
 * gets 0 must drop the packet without looking at any of it — this is the only
 * place a hostile datagram is filtered. */
SHN_FN int ShnParseHeader(const shn_u8* b, int len, shn_u8* outType,
                          shn_u16* outPayloadLen, shn_u32* outSession)
{
    int off = 4;
    if (len < SHNET_HDR_SIZE)
    {
        return 0;
    }
    if (b[0] != SHNET_MAGIC0 || b[1] != SHNET_MAGIC1 ||
        b[2] != SHNET_MAGIC2 || b[3] != SHNET_MAGIC3)
    {
        return 0;
    }
    if (ShnGetU8(b, &off) != SHNET_PROTO_VER)
    {
        return 0;
    }
    *outType       = ShnGetU8(b, &off);
    *outPayloadLen = ShnGetU16(b, &off);
    *outSession    = ShnGetU32(b, &off);
    if (*outPayloadLen > SHNET_MAX_PAYLOAD ||
        SHNET_HDR_SIZE + (int)*outPayloadLen > len)
    {
        return 0;
    }
    return 1;
}

/* Non-cryptographic. This exists so a private server can require a shared word
 * without either end linking a hash library, and to keep the plaintext off the
 * wire — it is NOT a defence against anyone who can watch the traffic. */
SHN_FN shn_u32 ShnHashPassword(const char* s)
{
    shn_u32 h = 2166136261u;
    if (s == 0)
    {
        return 0;
    }
    while (*s)
    {
        h ^= (shn_u8)*s++;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* Printable ASCII only, in place: anything a peer or server names (players,
 * rooms, servers, MOTD, chat) reaches the font, the UI and the log, and must
 * not carry control bytes, newlines or bytes the font has no glyph for. */
SHN_FN void ShnSanitizeText(char* s)
{
    if (s == 0) return;
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) *s = '?';
    }
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

/* HELLO = ver u16, pad u8, chara u8, reconnect u32, name, build, proof u32,
 * cookie u32. A HELLO without the cookie field is an outdated client. The
 * server answers a HELLO whose cookie is not the one it would issue to that
 * address with a 16-byte CHALLENGE (smaller than the HELLO, so it cannot be
 * used to amplify), and allocates nothing until the cookie comes back, which
 * proves the sender really receives at the address it claims. */
/* Co-op peer messages own 0x50..0x5F; the relay forwards that whole range
 * (framing checked) so a new S_* type needs no server change. The ones that
 * change everyone's game may only come from the world's owner. */
/* SHNET_MSG_S_CTRL kinds (first payload byte). Host-only kinds are checked
 * by the receiver against the session host. */
enum
{
    SHNET_CTRL_PAUSE     = 1, /* host -> all: u8 paused */
    SHNET_CTRL_PAUSE_REQ = 2, /* member -> host: please pause */
    SHNET_CTRL_PERM      = 3, /* host -> all: u8 count, then {u64 id, u8 flags} */
    SHNET_CTRL_VOICE     = 4, /* member -> all: u8 chara, u8 line, u16 xaIdx */
    SHNET_CTRL_CUTSCENE  = 5, /* member -> all: u8 map, u8 row, s32 x,y,z, s16 rot (repeated while the scene runs) */
    SHNET_CTRL_DIE       = 6, /* member -> all: u8 map; I died in the alley (map0_s00), die with me */
    SHNET_CTRL_GATE      = 7  /* member -> all: u8 map, u8 row, u8 state (1 waiting at a story door, 2 went through, 3 host: come through with me) */
};
#define SHNET_PERM_CHEATS 1    /* this guest may use the console, quick-menu cheats, binds */

static inline void ShnPutU64(shn_u8* b, int* off, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; i++) { b[(*off)++] = (shn_u8)(v & 0xFF); v >>= 8; }
}
static inline unsigned long long ShnGetU64(const shn_u8* b, int* off)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | b[*off + i];
    *off += 8;
    return v;
}

#define SHNET_MSG_S_FIRST 0x50
#define SHNET_MSG_S_LAST  0x5F
#define SHNET_S_HOST_ONLY(t) ((t) == SHNET_MSG_S_WORLD || (t) == SHNET_MSG_S_KICK || \
                              (t) == SHNET_MSG_S_NPC_SNAP || (t) == SHNET_MSG_S_NPC_ATK || \
                              (t) == SHNET_MSG_S_WORLD_FL)
/* Types the session hands to the game thread whole (ShSession_TakeGame). */
#define SHNET_S_IS_GAME(t)   ((t) >= SHNET_MSG_S_NPC_SNAP && (t) <= SHNET_MSG_S_LAST)

#define SHNET_HELLO_LEN_OLD (2 + 1 + 1 + 4 + SHNET_NAME_MAX + SHNET_BUILD_MAX + 4)
#define SHNET_HELLO_LEN     (SHNET_HELLO_LEN_OLD + 4)

/* SipHash-2-4 (Aumasson and Bernstein), the standard keyed hash for exactly
 * this: short inputs, a secret key, outputs an attacker must not predict. */
#define SHN_ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))
#define SHN_SIPROUND(v0, v1, v2, v3) do { \
    v0 += v1; v1 = SHN_ROTL64(v1, 13); v1 ^= v0; v0 = SHN_ROTL64(v0, 32); \
    v2 += v3; v3 = SHN_ROTL64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = SHN_ROTL64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = SHN_ROTL64(v1, 17); v1 ^= v2; v2 = SHN_ROTL64(v2, 32); } while (0)

SHN_FN unsigned long long ShnSipHash(const shn_u8 key[16], const void* data, int len)
{
    const shn_u8*      in = (const shn_u8*)data;
    unsigned long long k0 = 0, k1 = 0, m, b;
    unsigned long long v0, v1, v2, v3;
    int                i, left;

    for (i = 0; i < 8; i++)
    {
        k0 |= (unsigned long long)key[i] << (8 * i);
        k1 |= (unsigned long long)key[8 + i] << (8 * i);
    }
    v0 = 0x736f6d6570736575ULL ^ k0;
    v1 = 0x646f72616e646f6dULL ^ k1;
    v2 = 0x6c7967656e657261ULL ^ k0;
    v3 = 0x7465646279746573ULL ^ k1;
    if (len < 0) len = 0;
    b = (unsigned long long)len << 56;
    for (; len >= 8; len -= 8, in += 8)
    {
        m = 0;
        for (i = 0; i < 8; i++) m |= (unsigned long long)in[i] << (8 * i);
        v3 ^= m;
        SHN_SIPROUND(v0, v1, v2, v3);
        SHN_SIPROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    left = len;
    for (i = 0; i < left; i++) b |= (unsigned long long)in[i] << (8 * i);
    v3 ^= b;
    SHN_SIPROUND(v0, v1, v2, v3);
    SHN_SIPROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    SHN_SIPROUND(v0, v1, v2, v3);
    SHN_SIPROUND(v0, v1, v2, v3);
    SHN_SIPROUND(v0, v1, v2, v3);
    SHN_SIPROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

/* What a client sends in HELLO's proof field: the password bound to this
 * server's cookie, so a captured HELLO is useless against any other cookie
 * and the password itself never crosses the wire. 0 = no password. Weak
 * passwords can still be guessed offline from a capture; that is the limit
 * of any shared-secret scheme without a PAKE. */
SHN_FN shn_u32 ShnPasswordProof(const char* pw, shn_u32 cookie)
{
    static const shn_u8 kDeriveA[16] = { 'S','H','O','1','-','p','w','-','d','e','r','i','v','e','-','A' };
    static const shn_u8 kDeriveB[16] = { 'S','H','O','1','-','p','w','-','d','e','r','i','v','e','-','B' };
    shn_u8              key[16];
    shn_u8              c[8];
    unsigned long long  h0, h1;
    int                 n = 0, i;

    if (pw == 0 || pw[0] == 0)
    {
        return 0;
    }
    while (pw[n] && n < 256) n++;
    h0 = ShnSipHash(kDeriveA, pw, n);
    h1 = ShnSipHash(kDeriveB, pw, n);
    for (i = 0; i < 8; i++)
    {
        key[i]     = (shn_u8)(h0 >> (8 * i));
        key[8 + i] = (shn_u8)(h1 >> (8 * i));
    }
    for (i = 0; i < 4; i++) c[i] = (shn_u8)(cookie >> (8 * i));
    c[4] = 'p'; c[5] = 'r'; c[6] = 'o'; c[7] = 'f';
    h0 = ShnSipHash(key, c, 8) & 0xFFFFFFFFULL;
    return h0 ? (shn_u32)h0 : 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_PROTO_H */
