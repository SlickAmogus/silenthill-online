/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_session.c - Steam lobby lifecycle and the peer-to-peer session.
 *
 * See sh_net_session.h for what a session is and why it is separate from the
 * master-server world.
 *
 * The shape is the same one the rest of the client already uses, because it
 * works: the game thread sets request flags under a lock, the worker services
 * them, and the worker publishes a copy the game thread reads without locking.
 * Steam is touched from the worker and nowhere else.
 */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "sh_net_proto.h"
#include "sh_net.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"

#define ABS_I(v) ((v) < 0 ? -(v) : (v))
/* Sfx_Base (sfx_id_enum.h): ids above it index g_Vab_InfoTable[420]. */
#define Sfx_Base_NET 1280
/* MapIdx_MAPX_S00 (map.h): the last map index the game can load. */
#define MAP_IDX_LAST 44
#define CLAMP_COORD(v) ((v) > SHNET_COORD_MAX ? SHNET_COORD_MAX : (v) < -SHNET_COORD_MAX ? -SHNET_COORD_MAX : (v))
#include "sh_net_steam.h"
#include "sh_net_coopudp.h"
#include "sh_net_coop.h"
#include "pc_config.h"
#include "sh_log.h"

/* Which transport this co-op session is riding. Steam P2P by default; switched to
 * the relay server when the player hosts/joins through a server. */
#define COOP_BACKEND_STEAM 0
#define COOP_BACKEND_UDP   1
static int s_backend = COOP_BACKEND_STEAM;

/* Transport primitives, dispatched to the active backend. The session logic
 * below is written against these, so it is identical for Steam and the server. */
static unsigned long long Tp_SelfId(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_SelfId() : ShSteam_SelfId(); }
static int Tp_MemberCount(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_MemberCount() : ShSteam_MemberCount(); }
static unsigned long long Tp_Member(int i)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Member(i) : ShSteam_Member(i); }
static const char* Tp_NameOf(unsigned long long id)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_NameOf(id) : ShSteam_NameOf(id); }
static int Tp_IsHost(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_IsHost() : ShSteam_IsLobbyOwner(); }
static unsigned long long Tp_HostId(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_HostId() : ShSteam_LobbyOwner(); }
static int Tp_Active(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Active() : (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN); }
static unsigned long long Tp_LobbyId(void)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_RoomId() : ShSteam_LobbyId(); }
static void Tp_Send(unsigned long long to, const shn_u8* buf, int len, int reliable)
{ if (s_backend == COOP_BACKEND_UDP) CoopUdp_Send(to, buf, len); else ShSteam_Send(to, buf, len, reliable); }
static int Tp_Recv(unsigned long long* from, shn_u8* buf, int cap)
{ return s_backend == COOP_BACKEND_UDP ? CoopUdp_Recv(from, buf, cap) : ShSteam_Recv(from, buf, cap); }

/* Round trip to each member, and how long before a silent member is no longer
 * called linked. A session is a handful of friends, so this can be leisurely. */
#define SESSION_PING_MS   2000
#define SESSION_LINK_MS   8000
#define SESSION_HELLO_MS  5000
#define SESSION_POS_MS    14   /* co-op pose: every game frame, up to ~60 Hz */
#define SESSION_DELAY_MIN 30   /* render-behind window bounds, ms; the window itself */
#define SESSION_DELAY_MAX 200  /* adapts to each member's send interval and jitter */
#define SESSION_EXTRAP_MS 100  /* past the newest sample, coast this long, then hold */
#define SESSION_WORLD_MS  1000 /* host re-tells guests to boot, until they arrive */

/* Requests, and the published copy. Both under s_lock. */
static SDL_mutex* s_lock;

static struct
{
    int                wantHost;
    int                wantLeave;
    int                wantInvite;
    unsigned long long wantJoin;

    char               presenceArea[64];
    int                presenceMap;
    int                presenceDirty;

    /* Local player pose for the co-op presence broadcast. poseValid is cleared
     * when not in a map so no pose goes out from the menus. */
    int                poseValid;
    int                poseMap, poseChara, poseFlags;
    ShSessionPose      pose;
    unsigned int       poseSeq;     /* bumps once per published game frame */

    int                wantWorldMap; /* host: send S_WORLD(map) to guests; -1 none */

    int                hostGuestDebug; /* host: allow joined players debug controls (0 default) */

    /* Co-op over a relay server (vs Steam). Set by the server host/join/list
     * requests; the Tick arms the UDP backend from them. */
    char               coopHost[80];
    int                coopPort;
    int                coopMax;
    int                coopJoinCode;
    int                coopListen;     /* host the relay in-process (this PC) */
    int                coopHidden;     /* server room not listed (join by code) */
    int                coopUseWorld;   /* ride the living-world link if connected */
    int                wantHostServer;
    int                wantJoinServer;
    int                wantListServer;
} s_req;

static struct
{
    int                active;
    int                isHost;
    unsigned long long lobbyId;
    int                memberCount;
    unsigned long long hostId;
    ShSessionMember    members[SHSESSION_MAX_MEMBERS];
    char               status[128];
    int                worldReq;    /* guest: a map the host said to boot into, -1 none */
    int                guestDebug;  /* host granted joined players debug controls */

    /* Open server rooms from the last list request, for the Join browser. */
    struct { unsigned short code; unsigned char players, max; char host[SHSESSION_NAME_MAX]; } rooms[16];
    int                roomCount;
} s_pub;

/* Worker-private. */
static ShSessionMember s_members[SHSESSION_MAX_MEMBERS];
static int             s_memberCount;
static unsigned int    s_lastPingMs;
static unsigned int    s_lastHelloMs;
static unsigned int    s_lastSeenMs[SHSESSION_MAX_MEMBERS];
static unsigned int    s_pingSentMs[SHSESSION_MAX_MEMBERS];
static unsigned int    s_lastPosMs;   /* last co-op pose broadcast */
static unsigned int    s_lastPosSeq;  /* the published frame it carried */
static int             s_worldReqIn = -1; /* a map the host told us to boot into */
static int             s_declineWorld = 0; /* guest chose to leave: ignore the host's
                                            * resent S_WORLD so it cannot drag them back,
                                            * until they intentionally join/host again */
static int             s_enabled;

/* Co-op shared-item queues, both under s_lock. Out: local pickups waiting for the
 * worker to broadcast. In: pickups received from members, waiting for the game
 * thread to grant. A handful of pickups a room, so a small ring is plenty. */
#define SESSION_ITEM_Q 32
typedef struct { shn_u8 id; shn_u16 n; } ShSessionItem;
static ShSessionItem s_itemOut[SESSION_ITEM_Q];
static int           s_itemOutCount;
static ShSessionItem s_itemIn[SESSION_ITEM_Q];
static int           s_itemInCount;

/* Friendly fire: a hit the local player landed on a remote player's proxy,
 * sent to THAT player, who applies it to their own character (victim-
 * authoritative -- the attacker never writes anyone else's health). In: hits
 * landed on us, for the game thread to apply. */
#define SESSION_DMG_Q 16
typedef struct
{
    unsigned long long to;
    int                amount;        /* Q12 HP, pre-difficulty, already PvP-scaled */
    int                kbx, kby, kbz; /* Q12 knockback vector */
    unsigned char      reaction;      /* enemy attack id the victim flinches to */
    short              yaw;           /* attacker heading, for the flinch direction */
} ShSessionDmg;
static ShSessionDmg s_dmgOut[SESSION_DMG_Q];
static int          s_dmgOutCount;
static ShSessionDmg s_dmgIn[SESSION_DMG_Q];
static int          s_dmgInCount;

/* Host: the map guests should be booted into, kept persistent (not one-shot) so a
 * player who joins AFTER the host started still gets sent in -- join-in-progress.
 * s_worldSent[i] is "member i has been sent the current world"; a map change
 * clears it so everyone is re-sent, and a fresh joiner's stays 0 so only they
 * are sent. -1 = host is not in a game yet (lobby only). */
static int           s_worldMapCur = -1;
static int           s_worldSent[SHSESSION_MAX_MEMBERS];
static unsigned int  s_lastWorldSendMs;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ShSession_Init(void)
{
    if (s_lock)
    {
        return;
    }
    s_lock = SDL_CreateMutex();
    if (!s_lock)
    {
        SH_DBG("[SESSION] SDL_CreateMutex failed: %s", SDL_GetError());
        return;
    }
    memset(&s_req, 0, sizeof(s_req));
    memset(&s_pub, 0, sizeof(s_pub));
    s_req.presenceMap  = -1;
    s_req.wantWorldMap = -1;
    s_pub.worldReq     = -1;

    /* The server (UDP) co-op transport does not need Steam, so it always comes
     * up. Steam is a second transport on top: ShSteam_Init fails gracefully
     * (returns 0) with no steam_api64.dll or no Steam running, and then only the
     * "join a friend" path is unavailable -- server rooms still work. */
    CoopUdp_Init();

    s_enabled = ShSteam_Init((unsigned int)g_PcConfig.onlineSteamAppId);
    if (!s_enabled)
    {
        SDL_strlcpy(s_pub.status, "Steam unavailable (server co-op still works)",
                    sizeof(s_pub.status));
    }
    /* A lobby id on the command line means the player clicked Join on a
     * friend before the game existed; honour it as soon as we are up. */
    else if (g_PcConfig.onlineSteamAutoHost)
    {
        SDL_LockMutex(s_lock);
        s_req.wantHost = 1;
        SDL_UnlockMutex(s_lock);
    }
}

void ShSession_Shutdown(void)
{
    if (s_enabled)
    {
        int i;
        /* Tell the others rather than letting them time out. */
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE];
            if (s_members[i].steamId == ShSteam_SelfId())
            {
                continue;
            }
            ShnPutHeader(buf, SHNET_MSG_S_BYE, 0, 0);
            ShSteam_Send(s_members[i].steamId, buf, SHNET_HDR_SIZE, 0);
        }
        ShSteam_Shutdown();
        s_enabled = 0;
    }
    CoopUdp_Shutdown();
    if (s_lock)
    {
        SDL_DestroyMutex(s_lock);
        s_lock = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Requests from the game thread                                       */
/* ------------------------------------------------------------------ */

void ShSession_RequestHost(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantHost = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestHostServer(const char* host, int port, int maxPlayers, int hidden, int useWorld)
{
    if (!s_lock || !host || !host[0]) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopMax        = maxPlayers;
    s_req.coopListen     = 0;
    s_req.coopHidden     = hidden ? 1 : 0;
    s_req.coopUseWorld   = useWorld ? 1 : 0;
    s_req.wantHostServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestHostListen(int port, int maxPlayers)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.coopHost[0]    = '\0'; /* loopback; the relay runs in-process */
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopMax        = maxPlayers;
    s_req.coopListen     = 1;
    s_req.coopHidden     = 0;
    s_req.coopUseWorld   = 0;
    s_req.wantHostServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestJoinServer(const char* host, int port, int code, int useWorld)
{
    if (!s_lock || !host || !host[0] || code < 0) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopUseWorld   = useWorld ? 1 : 0;
    s_req.coopJoinCode   = code;
    s_req.wantJoinServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestListServer(const char* host, int port, int useWorld)
{
    if (!s_lock || !host || !host[0]) return;
    SDL_LockMutex(s_lock);
    SDL_strlcpy(s_req.coopHost, host, sizeof(s_req.coopHost));
    s_req.coopPort       = port ? port : SHNET_DEFAULT_PORT;
    s_req.coopUseWorld   = useWorld ? 1 : 0;
    s_req.wantListServer = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestJoin(unsigned long long lobbyId)
{
    if (!s_lock || !lobbyId) return;
    SDL_LockMutex(s_lock);
    s_req.wantJoin = lobbyId;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestLeave(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantLeave = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_RequestInvite(void)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantInvite = 1;
    SDL_UnlockMutex(s_lock);
}

void ShSession_PublishPresence(const char* area, int mapIdx)
{
    if (!s_lock || !area) return;
    SDL_LockMutex(s_lock);
    if (mapIdx != s_req.presenceMap || strcmp(area, s_req.presenceArea) != 0)
    {
        SDL_strlcpy(s_req.presenceArea, area, sizeof(s_req.presenceArea));
        s_req.presenceMap   = mapIdx;
        s_req.presenceDirty = 1;
    }
    SDL_UnlockMutex(s_lock);
}

void ShSession_PublishLocalPos(int mapIdx, int charaId, int flags, const ShSessionPose* pose)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    if (mapIdx < 0 || !pose)
    {
        s_req.poseValid = 0;
    }
    else
    {
        s_req.poseValid = 1;
        s_req.poseMap   = mapIdx;
        s_req.poseChara = charaId;
        s_req.poseFlags = flags;
        s_req.pose      = *pose;
        s_req.poseSeq++;
    }
    SDL_UnlockMutex(s_lock);
}

/* Position and heading come from the caller; the body shows one sample's
 * exact pose. Samples arrive every frame, so the nearest one IS the frame the
 * sender drew, and blending two already-blended poses has no single
 * Anim_BoneUpdate form anyway. */
static void ShSession_PoseBody(const ShSessionPose* s, ShSessionPoseNow* out)
{
    out->lowKf0 = s->kf[0][0]; out->lowKf1 = s->kf[0][1]; out->lowAlpha = s->alpha[0];
    out->upKf0  = s->kf[1][0]; out->upKf1  = s->kf[1][1]; out->upAlpha  = s->alpha[1];
    out->weaponAnm = s->weaponAnm; out->heldItem = s->heldItem; out->handBone = s->handBone;
}

static void ShSession_PoseHold(const ShSessionPose* s, ShSessionPoseNow* out)
{
    out->x = s->x; out->y = s->y; out->z = s->z; out->rotY = s->rotY;
    ShSession_PoseBody(s, out);
}

static int ShSession_IsCut(const ShSessionPose* a, const ShSessionPose* b)
{
    return (b->ms - a->ms) > 400u ||
           ABS_I(b->x - a->x) > (3 << 12) || ABS_I(b->z - a->z) > (3 << 12) ||
           ABS_I(b->y - a->y) > (3 << 12);
}

int ShSession_MemberPoseNow(int i, ShSessionPoseNow* out)
{
    ShSessionPose ring[SHSESSION_POSE_RING];
    int           n, a, b, t, delay;
    unsigned int  at, span;

    if (!s_lock || !out) return 0;
    SDL_LockMutex(s_lock);
    n = (i >= 0 && i < s_pub.memberCount) ? s_pub.members[i].ringCount : 0;
    delay = (n > 0) ? s_pub.members[i].delayMs : 0;
    if (n > 0) memcpy(ring, s_pub.members[i].ring, sizeof(ring[0]) * (size_t)n);
    SDL_UnlockMutex(s_lock);
    if (n <= 0) return 0;

    at = ShNetPlat_Millis() - (unsigned int)delay;
    for (b = 0; b < n && (int)(ring[b].ms - at) <= 0; b++) {}
    if (b == 0)
    {
        ShSession_PoseHold(&ring[0], out);
        return 1;
    }
    if (b == n)
    {
        /* Out of samples (a hitch or a lost packet): coast on the last
         * velocity briefly, then hold. Coasting hides a single loss; holding
         * after that avoids sailing through walls on a real stall. */
        const ShSessionPose* p1 = &ring[n - 1];
        unsigned int         over = at - p1->ms;
        ShSession_PoseHold(p1, out);
        if (n >= 2 && over <= SESSION_EXTRAP_MS && !ShSession_IsCut(&ring[n - 2], p1))
        {
            const ShSessionPose* p0 = &ring[n - 2];
            unsigned int         sp = p1->ms - p0->ms;
            if (sp > 0)
            {
                out->x = p1->x + (int)(((long long)(p1->x - p0->x) * (long long)over) / (long long)sp);
                out->y = p1->y + (int)(((long long)(p1->y - p0->y) * (long long)over) / (long long)sp);
                out->z = p1->z + (int)(((long long)(p1->z - p0->z) * (long long)over) / (long long)sp);
            }
        }
        return 1;
    }
    a    = b - 1;
    span = ring[b].ms - ring[a].ms;
    t    = (span > 0) ? (int)(((unsigned long long)(at - ring[a].ms) << 12) / span) : 4096;
    if (t > 4096) t = 4096;
    if (ShSession_IsCut(&ring[a], &ring[b]))
    {
        /* A long gap or a jump (respawn, door) is a cut, not motion. */
        ShSession_PoseHold((t < 2048) ? &ring[a] : &ring[b], out);
        return 1;
    }

    out->x = ring[a].x + (int)(((long long)(ring[b].x - ring[a].x) * t) >> 12);
    out->y = ring[a].y + (int)(((long long)(ring[b].y - ring[a].y) * t) >> 12);
    out->z = ring[a].z + (int)(((long long)(ring[b].z - ring[a].z) * t) >> 12);
    {
        int d = ((int)(unsigned short)ring[b].rotY - (int)(unsigned short)ring[a].rotY) & 0xFFF;
        if (d >= 2048) d -= 4096;
        out->rotY = (short)(((int)ring[a].rotY + ((d * t) >> 12)) & 0xFFF);
    }
    ShSession_PoseBody((t < 2048) ? &ring[a] : &ring[b], out);
    return 1;
}

int ShSession_MemberTakeSfx(int i, unsigned int* serial, ShSessionSfxEv* out, int max)
{
    int          k, got = 0;
    unsigned int at;

    if (!s_lock || !serial || !out || max <= 0) return 0;
    SDL_LockMutex(s_lock);
    if (i >= 0 && i < s_pub.memberCount)
    {
        const ShSessionMember* m = &s_pub.members[i];
        at = ShNetPlat_Millis() - (unsigned int)m->delayMs;
        for (k = 0; k < m->sfxQn && got < max; k++)
        {
            if ((int)(m->sfxSerial[k] - *serial) <= 0) continue;
            if ((int)(m->sfxQ[k].ms - at) > 0) break; /* queue is in time order */
            out[got++] = m->sfxQ[k];
            *serial    = m->sfxSerial[k];
        }
    }
    SDL_UnlockMutex(s_lock);
    return got;
}

void ShSession_RequestWorld(int mapIdx)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.wantWorldMap = mapIdx;
    SDL_UnlockMutex(s_lock);
}

int ShSession_TakeWorldRequest(void)
{
    int map = -1;
    if (!s_lock) return -1;
    SDL_LockMutex(s_lock);
    map            = s_pub.worldReq;
    s_pub.worldReq = -1;
    SDL_UnlockMutex(s_lock);
    return map;
}

void ShSession_QueueItem(int itemId, int count)
{
    if (!s_lock || itemId < 0 || itemId > 255 || count <= 0) return;
    SDL_LockMutex(s_lock);
    if (s_itemOutCount < SESSION_ITEM_Q)
    {
        s_itemOut[s_itemOutCount].id = (shn_u8)itemId;
        s_itemOut[s_itemOutCount].n  = (shn_u16)(count > 65535 ? 65535 : count);
        s_itemOutCount++;
    }
    SDL_UnlockMutex(s_lock);
}

int ShSession_TakeItem(int* itemId, int* count)
{
    int got = 0;
    if (!s_lock) return 0;
    SDL_LockMutex(s_lock);
    if (s_itemInCount > 0)
    {
        if (itemId) *itemId = s_itemIn[0].id;
        if (count)  *count  = s_itemIn[0].n;
        s_itemInCount--;
        memmove(s_itemIn, s_itemIn + 1, (size_t)s_itemInCount * sizeof(s_itemIn[0]));
        got = 1;
    }
    SDL_UnlockMutex(s_lock);
    return got;
}

unsigned long long ShSession_SelfId(void)
{
    return Tp_SelfId();
}

void ShSession_QueueDamage(unsigned long long toId, int amount, int kbx, int kby, int kbz,
                           int reaction, short yaw)
{
    if (!s_lock || !toId || amount <= 0) return;
    SDL_LockMutex(s_lock);
    if (s_dmgOutCount < SESSION_DMG_Q)
    {
        ShSessionDmg* d = &s_dmgOut[s_dmgOutCount++];
        d->to = toId; d->amount = amount;
        d->kbx = kbx; d->kby = kby; d->kbz = kbz;
        d->reaction = (unsigned char)reaction; d->yaw = yaw;
    }
    SDL_UnlockMutex(s_lock);
}

int ShSession_TakeDamage(int* amount, int* kbx, int* kby, int* kbz, int* reaction, short* yaw)
{
    int got = 0;
    if (!s_lock) return 0;
    SDL_LockMutex(s_lock);
    if (s_dmgInCount > 0)
    {
        const ShSessionDmg* d = &s_dmgIn[0];
        if (amount)   *amount   = d->amount;
        if (kbx)      *kbx      = d->kbx;
        if (kby)      *kby      = d->kby;
        if (kbz)      *kbz      = d->kbz;
        if (reaction) *reaction = d->reaction;
        if (yaw)      *yaw      = d->yaw;
        s_dmgInCount--;
        memmove(s_dmgIn, s_dmgIn + 1, (size_t)s_dmgInCount * sizeof(s_dmgIn[0]));
        got = 1;
    }
    SDL_UnlockMutex(s_lock);
    return got;
}

/* ------------------------------------------------------------------ */
/* Published state                                                     */
/* ------------------------------------------------------------------ */

int ShSession_Active(void)
{
    return s_pub.active;
}

int ShSession_MemberCount(void)
{
    return s_pub.memberCount;
}

const ShSessionMember* ShSession_Member(int i)
{
    return (i >= 0 && i < s_pub.memberCount) ? &s_pub.members[i] : NULL;
}

unsigned long long ShSession_LobbyId(void)
{
    return s_pub.lobbyId;
}

int ShSession_IsHost(void)
{
    return s_pub.isHost;
}

unsigned long long ShSession_HostId(void)
{
    return s_pub.hostId;
}

void ShSession_SetGuestDebug(int on)
{
    if (!s_lock) return;
    SDL_LockMutex(s_lock);
    s_req.hostGuestDebug = on ? 1 : 0;
    SDL_UnlockMutex(s_lock);
}

int ShSession_GuestDebugGranted(void)
{
    return s_pub.guestDebug;
}

void ShSession_StatusLine(char* out, int cap)
{
    if (!out || cap <= 0) return;
    SDL_strlcpy(out, s_pub.status, (size_t)cap);
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

static int ShSession_IndexOf(unsigned long long id)
{
    int i;
    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i].steamId == id)
        {
            return i;
        }
    }
    return -1;
}

/* Rebuild the worker's member table from the lobby, preserving what we have
 * already learned about anyone still in it. Steam is the authority on WHO is
 * present; the ping and the map come from our own traffic. */
static void ShSession_SyncMembers(void)
{
    ShSessionMember fresh[SHSESSION_MAX_MEMBERS];
    unsigned int    freshSeen[SHSESSION_MAX_MEMBERS];
    unsigned int    freshSent[SHSESSION_MAX_MEMBERS];
    int             freshWorld[SHSESSION_MAX_MEMBERS];
    int             n = Tp_MemberCount();
    int             count = 0;
    int             i;

    if (n > SHSESSION_MAX_MEMBERS)
    {
        n = SHSESSION_MAX_MEMBERS;
    }
    memset(fresh, 0, sizeof(fresh));
    memset(freshSeen, 0, sizeof(freshSeen));
    memset(freshSent, 0, sizeof(freshSent));
    memset(freshWorld, 0, sizeof(freshWorld)); /* a new member has not been sent the world */

    for (i = 0; i < n; i++)
    {
        unsigned long long id  = Tp_Member(i);
        int                old = ShSession_IndexOf(id);
        const char*        nm;

        if (!id)
        {
            continue;
        }
        if (old >= 0)
        {
            fresh[count]      = s_members[old];
            freshSeen[count]  = s_lastSeenMs[old];
            freshSent[count]  = s_pingSentMs[old];
            freshWorld[count] = s_worldSent[old];
        }
        else
        {
            fresh[count].steamId = id;
            fresh[count].pingMs  = -1;
            fresh[count].mapIdx  = -1;
            fresh[count].linked  = 0;
        }
        nm = Tp_NameOf(id);
        if (nm && nm[0])
        {
            SDL_strlcpy(fresh[count].name, nm, SHSESSION_NAME_MAX);
        }
        else if (!fresh[count].name[0])
        {
            SDL_snprintf(fresh[count].name, SHSESSION_NAME_MAX, "%llu",
                         (unsigned long long)id);
        }
        count++;
    }

    memcpy(s_members, fresh, sizeof(s_members));
    memcpy(s_lastSeenMs, freshSeen, sizeof(s_lastSeenMs));
    memcpy(s_pingSentMs, freshSent, sizeof(s_pingSentMs));
    memcpy(s_worldSent, freshWorld, sizeof(s_worldSent));
    s_memberCount = count;
}

/* A member can send as fast as its link allows; logging every one of those
 * would let one hostile peer fill the disk. 20 lines per member per 10 s. */
static int ShSession_LogOk(int idx)
{
    static unsigned int s_win[SHSESSION_MAX_MEMBERS];
    static int          s_n[SHSESSION_MAX_MEMBERS];
    unsigned int        now = ShNetPlat_Millis();
    if (idx < 0 || idx >= SHSESSION_MAX_MEMBERS) return 0;
    if (now - s_win[idx] > 10000u) { s_win[idx] = now; s_n[idx] = 0; }
    return ++s_n[idx] <= 20;
}

/* Per-sender budget for messages that change OUR game (items, hits). */
static int ShSession_BudgetOk(int idx, unsigned int* winMs, int* used, int perSec)
{
    unsigned int now = ShNetPlat_Millis();
    if (idx < 0 || idx >= SHSESSION_MAX_MEMBERS) return 0;
    if (now - winMs[idx] >= 1000u) { winMs[idx] = now; used[idx] = 0; }
    return ++used[idx] <= perSec;
}

static void ShSession_SendHello(unsigned long long to)
{
    shn_u8 buf[SHNET_HDR_SIZE + SHNET_NAME_MAX + 4];
    int    off = SHNET_HDR_SIZE;
    int    map;

    SDL_LockMutex(s_lock);
    map = s_req.presenceMap;
    SDL_UnlockMutex(s_lock);

    ShnPutStr(buf, &off, ShSteam_PersonaName(), SHNET_NAME_MAX);
    ShnPutU8(buf, &off, (shn_u8)((map < 0 || map > 255) ? 0xFF : map));
    ShnPutU8(buf, &off, 0);
    ShnPutU16(buf, &off, 0);
    ShnPutHeader(buf, SHNET_MSG_S_HELLO, (shn_u16)(off - SHNET_HDR_SIZE), 0);
    Tp_Send(to, buf, off, 1);
}

static void ShSession_Receive(unsigned int now)
{
    shn_u8             buf[SHNET_MTU];
    unsigned long long from = 0;
    int                budget = 32;

    while (budget-- > 0)
    {
        shn_u8  type;
        shn_u16 payLen;
        shn_u32 session;
        int     idx;
        int     n = Tp_Recv(&from, buf, (int)sizeof(buf));

        if (n <= 0)
        {
            break;
        }
        if (!ShnParseHeader(buf, n, &type, &payLen, &session))
        {
            continue;
        }
        /* Only from someone the lobby says is here. ShSteam_AcceptSession
         * already refuses non-members, so this is the second of two checks
         * rather than the only one. */
        idx = ShSession_IndexOf(from);
        if (idx < 0)
        {
            continue;
        }
        s_lastSeenMs[idx]    = now;
        s_members[idx].linked = 1;

        switch (type)
        {
        case SHNET_MSG_S_HELLO:
        {
            int  off = 0;
            char nm[SHNET_NAME_MAX];
            if (payLen < SHNET_NAME_MAX + 4)
            {
                break;
            }
            ShnGetStr(buf + SHNET_HDR_SIZE, &off, SHNET_NAME_MAX, nm, SHNET_NAME_MAX);
            ShnSanitizeText(nm);
            /* The transport's own name for a member (Steam persona, relay
             * roster) wins: a self-announced name would let anyone call
             * themselves the host. */
            {
                const char* tn = Tp_NameOf(from);
                if (nm[0] && !(tn && tn[0]))
                {
                    SDL_strlcpy(s_members[idx].name, nm, SHSESSION_NAME_MAX);
                }
            }
            {
                int m = (int)ShnGetU8(buf + SHNET_HDR_SIZE, &off);
                s_members[idx].mapIdx = (m == 0xFF || m > MAP_IDX_LAST) ? -1 : m;
            }
            if (ShSession_LogOk(idx))
            {
                SH_DBG("[SESSION] %s is in the session (map %d)",
                       s_members[idx].name, s_members[idx].mapIdx);
            }
            break;
        }
        case SHNET_MSG_S_PING:
        {
            /* Echo the sender's clock straight back; they do the arithmetic,
             * so the two machines never have to agree on a time base. */
            shn_u8 out[SHNET_HDR_SIZE + 4];
            int    o  = SHNET_HDR_SIZE;
            int    ro = 0;
            shn_u32 stamp;
            if (payLen < 4)
            {
                break;
            }
            stamp = ShnGetU32(buf + SHNET_HDR_SIZE, &ro);
            ShnPutU32(out, &o, stamp);
            ShnPutHeader(out, SHNET_MSG_S_PONG, (shn_u16)(o - SHNET_HDR_SIZE), 0);
            Tp_Send(from, out, o, 0);
            break;
        }
        case SHNET_MSG_S_PONG:
        {
            int     ro = 0;
            shn_u32 stamp;
            if (payLen < 4)
            {
                break;
            }
            stamp = ShnGetU32(buf + SHNET_HDR_SIZE, &ro);
            {
                int rtt = (int)(now - stamp);
                if (rtt < 0)     rtt = 0;
                if (rtt > 9999)  rtt = 9999;
                s_members[idx].pingMs = rtt;
            }
            break;
        }
        case SHNET_MSG_S_BYE:
            if (ShSession_LogOk(idx)) SH_DBG("[SESSION] %s left the session", s_members[idx].name);
            s_members[idx].linked = 0;
            break;
        case SHNET_MSG_S_POS:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           m;
            if (payLen < 22)
            {
                break;
            }
            m                      = (int)ShnGetU8(p, &o);
            if (m > MAP_IDX_LAST) m = 0xFF;
            if (s_members[idx].mapIdx != ((m == 0xFF) ? -1 : m))
            {
                s_members[idx].ringCount = 0; /* another map's coordinates */
            }
            s_members[idx].mapIdx  = (m == 0xFF) ? -1 : m;
            s_members[idx].charaId = (int)ShnGetU8(p, &o);
            s_members[idx].flags   = (int)ShnGetU8(p, &o);
            (void)ShnGetU8(p, &o); /* pad */
            s_members[idx].x       = (int)ShnGetS32(p, &o);
            s_members[idx].y       = (int)ShnGetS32(p, &o);
            s_members[idx].z       = (int)ShnGetS32(p, &o);
            s_members[idx].rotY    = (short)ShnGetS16(p, &o);
            s_members[idx].anim    = (unsigned short)ShnGetU16(p, &o);
            s_members[idx].frame   = (unsigned short)ShnGetU16(p, &o);
            s_members[idx].poseMs  = now;
            /* Everything downstream (interpolation, distance tests, respawn
             * placement) assumes a coordinate inside the world. */
            s_members[idx].x = CLAMP_COORD(s_members[idx].x);
            s_members[idx].y = CLAMP_COORD(s_members[idx].y);
            s_members[idx].z = CLAMP_COORD(s_members[idx].z);
            {
                ShSessionMember* mb = &s_members[idx];
                ShSessionPose    smp;
                unsigned int     sent;
                int              d;
                int              nsfx = 0, k;
                memset(&smp, 0, sizeof(smp));
                smp.x = mb->x; smp.y = mb->y; smp.z = mb->z; smp.rotY = mb->rotY;
                smp.anim = mb->anim; smp.frame = mb->frame;
                if (payLen >= 42)
                {
                    smp.kf[0][0]  = (unsigned short)ShnGetU16(p, &o);
                    smp.kf[0][1]  = (unsigned short)ShnGetU16(p, &o);
                    smp.alpha[0]  = (unsigned short)ShnGetU16(p, &o);
                    smp.kf[1][0]  = (unsigned short)ShnGetU16(p, &o);
                    smp.kf[1][1]  = (unsigned short)ShnGetU16(p, &o);
                    smp.alpha[1]  = (unsigned short)ShnGetU16(p, &o);
                    smp.weaponAnm = (unsigned char)ShnGetU8(p, &o);
                    smp.heldItem  = (unsigned char)ShnGetU8(p, &o);
                    smp.handBone  = (unsigned char)ShnGetU8(p, &o);
                    nsfx          = (int)ShnGetU8(p, &o);
                    sent          = (unsigned int)ShnGetU32(p, &o);
                    if (nsfx > SHSESSION_POSE_SFX) nsfx = SHSESSION_POSE_SFX;
                    if (42 + nsfx * 6 > payLen) nsfx = (payLen - 42) / 6;
                    for (k = 0; k < nsfx; k++)
                    {
                        smp.sfx[k].seq   = (unsigned char)ShnGetU8(p, &o);
                        smp.sfx[k].sfx   = (unsigned short)ShnGetU16(p, &o);
                        smp.sfx[k].vol   = (unsigned char)ShnGetU8(p, &o);
                        smp.sfx[k].kind  = (unsigned char)ShnGetU8(p, &o);
                        smp.sfx[k].age4  = (unsigned char)ShnGetU8(p, &o);
                        smp.sfx[k].pitch = 0;
                    }
                }
                else
                {
                    smp.kf[0][0] = smp.kf[0][1] = smp.kf[1][0] = smp.kf[1][1] = smp.frame;
                    smp.weaponAnm = smp.heldItem = 0xFF;
                    sent = now;
                }
                /* Clamp what a peer can make us pose with: keyframes past the
                 * banks are bounded again where they are read (sh_net_ghost.c),
                 * the blend is a Q12 fraction. */
                if (smp.alpha[0] > 4096) smp.alpha[0] = 4096;
                if (smp.alpha[1] > 4096) smp.alpha[1] = 4096;
                /* Smallest one-way delay seen = the clock offset. It creeps up
                 * slowly so a route that got permanently slower is followed. */
                d = (int)(now - sent);
                if (!mb->clockOk || d < mb->clockOff)
                {
                    mb->clockOff = d;
                    mb->clockOk  = 1;
                }
                else if ((++mb->clockCreep & 3) == 0)
                {
                    mb->clockOff++;
                }
                smp.ms = sent + (unsigned int)mb->clockOff;
                if (mb->ringCount == 0 || (int)(smp.ms - mb->ring[mb->ringCount - 1].ms) > 0)
                {
                    /* Render window = one send interval plus twice the jitter:
                     * wide enough that the next sample is nearly always in
                     * before the clock reaches it, no wider. */
                    {
                        int dev = d - mb->clockOff;
                        int win;
                        if (dev < 0) dev = 0;
                        if (dev > 1000) dev = 1000;
                        mb->jitterQ4 += ((dev << 4) - mb->jitterQ4) >> 4;
                        if (mb->ringCount > 0)
                        {
                            int gap = (int)(smp.ms - mb->ring[mb->ringCount - 1].ms);
                            if (gap > 250) gap = 250;
                            mb->gapQ4 += ((gap << 4) - mb->gapQ4) >> 3;
                        }
                        else if (mb->gapQ4 == 0)
                        {
                            mb->gapQ4 = 17 << 4;
                        }
                        win = (mb->gapQ4 >> 4) + ((mb->jitterQ4 >> 4) * 2) + 8;
                        if (win < SESSION_DELAY_MIN) win = SESSION_DELAY_MIN;
                        if (win > SESSION_DELAY_MAX) win = SESSION_DELAY_MAX;
                        /* Move the window gently: a jump would replay or skip
                         * a slice of the partner's motion. */
                        if (mb->delayMs == 0)            mb->delayMs = win;
                        else if (win > mb->delayMs + 2)  mb->delayMs += 2;
                        else if (win < mb->delayMs - 1)  mb->delayMs -= 1;
                    }
                    if (mb->ringCount == SHSESSION_POSE_RING)
                    {
                        memmove(&mb->ring[0], &mb->ring[1], sizeof(mb->ring[0]) * (SHSESSION_POSE_RING - 1));
                        mb->ringCount--;
                    }
                    smp.sfxCount = 0;
                    mb->ring[mb->ringCount++] = smp;
                }
                /* Sounds: each rides several poses, play each seq once. A seq
                 * is new when it is ahead of the last one within half the
                 * byte's range. */
                for (k = 0; k < nsfx; k++)
                {
                    const ShSessionSfx* e = &smp.sfx[k];
                    ShSessionSfxEv      ev;
                    int                 ahead = (int)(unsigned char)(e->seq - mb->sfxLastSeq);
                    if (mb->sfxSeqOk && (ahead == 0 || ahead >= 128)) continue;
                    if (e->sfx <= Sfx_Base_NET || e->sfx >= Sfx_Base_NET + 420) continue;
                    mb->sfxSeqOk   = 1;
                    mb->sfxLastSeq = e->seq;
                    ev.ms    = smp.ms - (unsigned int)e->age4 * 4u;
                    ev.sfx   = e->sfx;
                    ev.vol   = e->vol;
                    ev.kind  = (unsigned char)(e->kind & 0x07);
                    ev.pitch = 0;
                    if (mb->sfxQn == SHSESSION_SFX_Q)
                    {
                        memmove(&mb->sfxQ[0], &mb->sfxQ[1], sizeof(mb->sfxQ[0]) * (SHSESSION_SFX_Q - 1));
                        memmove(&mb->sfxSerial[0], &mb->sfxSerial[1], sizeof(mb->sfxSerial[0]) * (SHSESSION_SFX_Q - 1));
                        mb->sfxQn--;
                    }
                    mb->sfxQ[mb->sfxQn]      = ev;
                    mb->sfxSerial[mb->sfxQn] = ++mb->sfxNextSerial;
                    mb->sfxQn++;
                }
                /* Drop sounds long past due: nothing will ever ask for them. */
                while (mb->sfxQn > 0 && (int)(now - mb->sfxQ[0].ms) > 2000)
                {
                    memmove(&mb->sfxQ[0], &mb->sfxQ[1], sizeof(mb->sfxQ[0]) * (SHSESSION_SFX_Q - 1));
                    memmove(&mb->sfxSerial[0], &mb->sfxSerial[1], sizeof(mb->sfxSerial[0]) * (SHSESSION_SFX_Q - 1));
                    mb->sfxQn--;
                }
            }
            break;
        }
        case SHNET_MSG_S_WORLD:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           m;
            if (payLen < 1)
            {
                break;
            }
            m = (int)ShnGetU8(p, &o);
            /* Only the world's owner picks the map, and only a real one: the
             * index reaches GameBoot_MapLoad, which selects a file by it. */
            if (from != Tp_HostId() || m > MAP_IDX_LAST)
            {
                break;
            }
            if (!s_declineWorld)
            {
                s_worldReqIn = m;
                if (ShSession_LogOk(idx)) SH_DBG("[SESSION] host says: boot into map %d", s_worldReqIn);
            }
            break;
        }
        case SHNET_MSG_S_ITEM:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            int           id, n;
            if (payLen < 4)
            {
                break;
            }
            id = (int)ShnGetU8(p, &o);
            (void)ShnGetU8(p, &o); /* pad */
            n  = (int)ShnGetU16(p, &o);
            {
                static unsigned int s_itWin[SHSESSION_MAX_MEMBERS];
                static int          s_itUsed[SHSESSION_MAX_MEMBERS];
                if (!ShSession_BudgetOk(idx, s_itWin, s_itUsed, 8)) break;
            }
            SDL_LockMutex(s_lock);
            if (s_itemInCount < SESSION_ITEM_Q && n > 0)
            {
                s_itemIn[s_itemInCount].id = (shn_u8)id;
                s_itemIn[s_itemInCount].n  = (shn_u16)n;
                s_itemInCount++;
            }
            SDL_UnlockMutex(s_lock);
            if (ShSession_LogOk(idx)) SH_DBG("[SESSION] %s picked up item %d x%d", s_members[idx].name, id, n);
            break;
        }
        case SHNET_MSG_S_DAMAGE:
        {
            const shn_u8* p = buf + SHNET_HDR_SIZE;
            int           o = 0;
            ShSessionDmg  d;
            if (payLen < 20)
            {
                break;
            }
            d.to       = 0;
            d.amount   = (int)ShnGetU32(p, &o);
            d.kbx      = (int)ShnGetU32(p, &o);
            d.kby      = (int)ShnGetU32(p, &o);
            d.kbz      = (int)ShnGetU32(p, &o);
            d.reaction = ShnGetU8(p, &o);
            (void)ShnGetU8(p, &o);
            d.yaw      = (short)ShnGetU16(p, &o);
            /* A hit only counts from someone who opted into friendly fire
             * themselves, is on our map, and is not firing faster than any
             * weapon can. */
            {
                static unsigned int s_dmWin[SHSESSION_MAX_MEMBERS];
                static int          s_dmUsed[SHSESSION_MAX_MEMBERS];
                int                 myMap;
                SDL_LockMutex(s_lock);
                myMap = s_req.poseValid ? s_req.poseMap : -1;
                SDL_UnlockMutex(s_lock);
                if (!(s_members[idx].flags & SHNET_PF_FFIRE) || s_members[idx].mapIdx != myMap ||
                    !ShSession_BudgetOk(idx, s_dmWin, s_dmUsed, 10))
                {
                    break;
                }
            }
            SDL_LockMutex(s_lock);
            if (s_dmgInCount < SESSION_DMG_Q)
            {
                s_dmgIn[s_dmgInCount++] = d;
            }
            SDL_UnlockMutex(s_lock);
            if (ShSession_LogOk(idx))
            {
                SH_DBG("[SESSION] %s hit me for %d.%03d HP", s_members[idx].name,
                       d.amount >> 12, ((d.amount & 0xFFF) * 1000) >> 12);
            }
            break;
        }
        default:
            break;
        }
    }
}

static void ShSession_Publish(void)
{
    int i;

    SDL_LockMutex(s_lock);
    s_pub.active      = Tp_Active();
    s_pub.isHost      = Tp_IsHost();
    s_pub.hostId      = Tp_HostId();
    s_pub.lobbyId     = Tp_LobbyId();
    s_pub.memberCount = s_memberCount;
    for (i = 0; i < s_memberCount; i++)
    {
        s_pub.members[i] = s_members[i];
    }
    if (s_backend == COOP_BACKEND_UDP) CoopUdp_StatusLine(s_pub.status, (int)sizeof(s_pub.status));
    else                               ShSteam_StatusLine(s_pub.status, (int)sizeof(s_pub.status));
    /* Hand a received world-boot request to the game thread. Only overwrite when
     * there is a new one, so a request already waiting to be taken is not lost. */
    if (s_worldReqIn >= 0)
    {
        s_pub.worldReq = s_worldReqIn;
        s_worldReqIn   = -1;
    }
    /* The host's debug-grant, read off the Steam lobby data (host reads back its
     * own value, a guest reads the host's). Steam path only for now; 0 otherwise. */
    if (s_pub.active && s_backend == COOP_BACKEND_STEAM)
    {
        const char* d  = ShSteam_GetLobbyData("dbg");
        s_pub.guestDebug = (d && d[0] == '1') ? 1 : 0;
    }
    else
    {
        s_pub.guestDebug = 0;
    }
    /* Open-room browser list (lives in the UDP module, filled by a list request;
     * independent of the active backend). */
    {
        int cap = (int)(sizeof(s_pub.rooms) / sizeof(s_pub.rooms[0]));
        int rc  = CoopUdp_RoomListCount();
        int k;
        if (rc > cap) rc = cap;
        for (k = 0; k < rc; k++)
        {
            unsigned short code = 0;
            int            players = 0, max = 0;
            CoopUdp_RoomListGet(k, &code, &players, &max, s_pub.rooms[k].host, SHSESSION_NAME_MAX);
            s_pub.rooms[k].code    = code;
            s_pub.rooms[k].players = (unsigned char)players;
            s_pub.rooms[k].max     = (unsigned char)max;
        }
        s_pub.roomCount = rc;
    }
    SDL_UnlockMutex(s_lock);
}

int ShSession_RoomCount(void)
{
    return s_pub.roomCount;
}

int ShSession_RoomGet(int i, int* code, int* players, int* max, char* host, int hostCap)
{
    if (i < 0 || i >= s_pub.roomCount) return 0;
    if (code)    *code    = s_pub.rooms[i].code;
    if (players) *players = s_pub.rooms[i].players;
    if (max)     *max     = s_pub.rooms[i].max;
    if (host && hostCap > 0) SDL_strlcpy(host, s_pub.rooms[i].host, (size_t)hostCap);
    return 1;
}

void ShSession_Tick(unsigned int nowMs)
{
    int                wantHost, wantLeave, wantInvite, presenceDirty;
    unsigned long long wantJoin;
    char               area[64];
    int                i;

    int                poseValid, poseMap, poseChara, poseFlags, wantWorldMap;
    ShSessionPose      pose;
    unsigned int       poseSeq;

    ShSessionItem      itemOut[SESSION_ITEM_Q];
    int                itemOutN = 0;
    int                q;
    int                hostGuestDebug;

    char               coopHost[80];
    int                coopPort, coopMax, coopJoinCode, coopListen, coopHidden, coopUseWorld;
    int                wantHostServer, wantJoinServer, wantListServer;

    if (!s_lock)
    {
        return;
    }

    /* Pump both transports: Steam for its overlay/invites (only if it came up),
     * the UDP relay always (it no-ops until a server is armed). */
    if (s_enabled) ShSteam_RunCallbacks();
    CoopUdp_Tick(nowMs);

    SDL_LockMutex(s_lock);
    wantHost      = s_req.wantHost;
    wantLeave     = s_req.wantLeave;
    wantInvite    = s_req.wantInvite;
    wantJoin      = s_req.wantJoin;
    presenceDirty = s_req.presenceDirty;
    SDL_strlcpy(area, s_req.presenceArea, sizeof(area));
    poseValid = s_req.poseValid;
    poseMap   = s_req.poseMap;  poseChara = s_req.poseChara; poseFlags = s_req.poseFlags;
    pose      = s_req.pose;
    poseSeq   = s_req.poseSeq;
    wantWorldMap        = s_req.wantWorldMap;
    hostGuestDebug      = s_req.hostGuestDebug; /* persistent: snapshot, do not reset */
    SDL_strlcpy(coopHost, s_req.coopHost, sizeof(coopHost));
    coopPort        = s_req.coopPort;
    coopMax         = s_req.coopMax;
    coopJoinCode    = s_req.coopJoinCode;
    coopListen      = s_req.coopListen;
    coopHidden      = s_req.coopHidden;
    coopUseWorld    = s_req.coopUseWorld;
    wantHostServer  = s_req.wantHostServer;
    wantJoinServer  = s_req.wantJoinServer;
    wantListServer  = s_req.wantListServer;
    itemOutN            = s_itemOutCount;
    if (itemOutN > 0)
    {
        memcpy(itemOut, s_itemOut, (size_t)itemOutN * sizeof(itemOut[0]));
        s_itemOutCount = 0;
    }
    s_req.wantHost      = 0;
    s_req.wantLeave     = 0;
    s_req.wantInvite    = 0;
    s_req.wantJoin      = 0;
    s_req.presenceDirty = 0;
    s_req.wantWorldMap  = -1;
    s_req.wantHostServer = 0;
    s_req.wantJoinServer = 0;
    s_req.wantListServer = 0;
    s_req.coopListen     = 0;
    s_req.coopHidden     = 0;
    s_req.coopUseWorld   = 0;
    SDL_UnlockMutex(s_lock);

    /* A Steam invite accepted in the overlay outranks an in-game Steam host,
     * but not an explicit server host/join the player just chose. */
    if (s_enabled && !wantHostServer && !wantJoinServer)
    {
        unsigned long long pending = ShSteam_TakePendingJoin();
        if (pending)
        {
            wantJoin = pending;
            wantHost = 0;
        }
    }

    if (wantLeave)
    {
        if (s_backend == COOP_BACKEND_UDP) CoopUdp_Leave();
        else if (s_enabled)                ShSteam_LeaveLobby();
        s_memberCount = 0;
        /* Stop the host's resent S_WORLD from re-booting us after we chose to
         * leave: drop any queued boot and ignore further ones until we rejoin. */
        s_declineWorld = 1;
        s_worldReqIn   = -1;
        s_pub.worldReq = -1;
    }

    /* Any intentional host/join re-arms world-boot acceptance (see s_declineWorld). */
    if (wantJoinServer || wantHostServer || wantJoin || wantHost)
    {
        s_declineWorld = 0;
    }

    /* Host/join routes the session onto a transport. A server choice switches to
     * the UDP backend (leaving any Steam lobby first); a Steam choice switches
     * back (leaving any server room first). The living world is untouched. */
    if (wantJoinServer)
    {
        if (s_enabled) ShSteam_LeaveLobby();
        CoopUdp_Disconnect(); /* drop any prior transport (relay thread / world ride / socket) */
        s_backend = COOP_BACKEND_UDP;
        if (coopUseWorld && ShNet_WorldConnected()) CoopUdp_UseWorldLink();
        else                                        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
        CoopUdp_JoinRoom((unsigned short)coopJoinCode);
    }
    else if (wantHostServer)
    {
        if (s_enabled) ShSteam_LeaveLobby();
        CoopUdp_Disconnect(); /* stop a prior This-PC relay / world ride before re-hosting */
        s_backend = COOP_BACKEND_UDP;
        if (coopListen)
        {
            CoopUdp_HostListen((unsigned short)coopPort, coopMax);
        }
        else
        {
            if (coopUseWorld && ShNet_WorldConnected()) CoopUdp_UseWorldLink();
            else                                        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
            CoopUdp_CreateRoom(coopMax, coopHidden);
        }
    }
    else if (wantJoin && s_enabled)
    {
        CoopUdp_Disconnect();
        s_backend = COOP_BACKEND_STEAM;
        ShSteam_JoinLobby(wantJoin);
    }
    else if (wantHost && s_enabled)
    {
        CoopUdp_Disconnect();
        s_backend = COOP_BACKEND_STEAM;
        ShSteam_CreateLobby(g_PcConfig.onlineSteamPublic ? SHSTEAM_LOBBY_PUBLIC
                                                         : SHSTEAM_LOBBY_FRIENDSONLY,
                            g_PcConfig.onlineSteamMaxPlayers);
    }

    /* A room-list query does not change the active transport. */
    if (wantListServer)
    {
        if (coopUseWorld && ShNet_WorldConnected()) CoopUdp_UseWorldLink();
        else                                        CoopUdp_SetServer(coopHost, (unsigned short)coopPort);
        CoopUdp_RequestRoomList();
    }

    if (wantInvite && s_backend == COOP_BACKEND_STEAM && s_enabled)
    {
        ShSteam_OpenInviteOverlay();
    }

    if (presenceDirty && s_backend == COOP_BACKEND_STEAM && s_enabled)
    {
        /* "status" is the line a friend sees under the game name. */
        ShSteam_SetRichPresence("status", area[0] ? area : "Silent Hill");
        ShSteam_SetRichPresence("steam_display", "#Status");
        ShSteam_SetRichPresence("area", area);
    }

    if (!Tp_Active())
    {
        if (s_memberCount)
        {
            s_memberCount = 0;
        }
        s_worldMapCur = -1;
        memset(s_worldSent, 0, sizeof(s_worldSent));
        ShSession_Publish();
        return;
    }

    ShSession_SyncMembers();

    /* Steam only: advertise the lobby (so a joiner can tell a Silent Hill session
     * from any other Spacewar lobby, its state, and the debug grant). The server
     * backend gets all of this from the room roster instead. */
    if (s_backend == COOP_BACKEND_STEAM && ShSteam_IsLobbyOwner())
    {
        ShSteam_SetLobbyData("game", "silenthill-online");
        ShSteam_SetLobbyData("proto", "1");
        ShSteam_SetLobbyData("state", s_worldMapCur >= 0 ? "ingame" : "lobby");
        ShSteam_SetLobbyData("dbg", hostGuestDebug ? "1" : "0");
    }

    /* Steam only: S_HELLO carries each peer's name/map. On the server backend the
     * roster already supplies names, so no hello is needed. */
    if (s_backend == COOP_BACKEND_STEAM && nowMs - s_lastHelloMs >= SESSION_HELLO_MS)
    {
        s_lastHelloMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            if (s_members[i].steamId != ShSteam_SelfId())
            {
                ShSession_SendHello(s_members[i].steamId);
            }
        }
    }

    if (nowMs - s_lastPingMs >= SESSION_PING_MS)
    {
        s_lastPingMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU32(buf, &off, nowMs);
            ShnPutHeader(buf, SHNET_MSG_S_PING, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 0);
            s_pingSentMs[i] = nowMs;
        }
    }

    /* Host -> guests: boot into the co-op map. The relay path is best-effort
     * (no reliable channel), so instead of sending once we RE-tell, on a 1s
     * throttle, every member who has not yet reported being on the map
     * (member.mapIdx, from their S_POS). This self-heals a dropped datagram and
     * a second game started in the same process -- both of which otherwise left
     * the guest stranded -- and stops for each guest the moment they arrive. */
    if (wantWorldMap >= 0 && wantWorldMap != s_worldMapCur)
    {
        s_worldMapCur = wantWorldMap;
        SH_DBG("[SESSION] co-op world is now map %d", s_worldMapCur);
    }
    if (Tp_IsHost() && s_worldMapCur >= 0 && nowMs - s_lastWorldSendMs >= SESSION_WORLD_MS)
    {
        s_lastWorldSendMs = nowMs;
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId() || s_members[i].mapIdx == s_worldMapCur)
            {
                continue;
            }
            ShnPutU8(buf, &off, (shn_u8)((s_worldMapCur > 255) ? 0xFF : s_worldMapCur));
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, 0);
            ShnPutHeader(buf, SHNET_MSG_S_WORLD, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 1);
            SH_DBG("[SESSION] world (map %d) -> %s (they report map %d)",
                   s_worldMapCur, s_members[i].name, s_members[i].mapIdx);
        }
    }

    /* Co-op presence: broadcast our pose, unreliable, throttled. */
    if (poseValid && poseSeq != s_lastPosSeq && nowMs - s_lastPosMs >= SESSION_POS_MS)
    {
        s_lastPosMs  = nowMs;
        s_lastPosSeq = poseSeq;
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 48 + SHSESSION_POSE_SFX * 6];
            int    off = SHNET_HDR_SIZE;
            int    k;
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU8(buf, &off, (shn_u8)((poseMap > 255) ? 0xFF : poseMap));
            ShnPutU8(buf, &off, (shn_u8)poseChara);
            ShnPutU8(buf, &off, (shn_u8)poseFlags);
            ShnPutU8(buf, &off, 0);
            ShnPutS32(buf, &off, pose.x);
            ShnPutS32(buf, &off, pose.y);
            ShnPutS32(buf, &off, pose.z);
            ShnPutS16(buf, &off, pose.rotY);
            ShnPutU16(buf, &off, pose.anim);
            ShnPutU16(buf, &off, pose.frame);
            ShnPutU16(buf, &off, pose.kf[0][0]);
            ShnPutU16(buf, &off, pose.kf[0][1]);
            ShnPutU16(buf, &off, pose.alpha[0]);
            ShnPutU16(buf, &off, pose.kf[1][0]);
            ShnPutU16(buf, &off, pose.kf[1][1]);
            ShnPutU16(buf, &off, pose.alpha[1]);
            ShnPutU8(buf, &off, pose.weaponAnm);
            ShnPutU8(buf, &off, pose.heldItem);
            ShnPutU8(buf, &off, pose.handBone);
            ShnPutU8(buf, &off, pose.sfxCount);
            ShnPutU32(buf, &off, (shn_u32)pose.ms);
            for (k = 0; k < pose.sfxCount && k < SHSESSION_POSE_SFX; k++)
            {
                ShnPutU8(buf, &off, pose.sfx[k].seq);
                ShnPutU16(buf, &off, pose.sfx[k].sfx);
                ShnPutU8(buf, &off, pose.sfx[k].vol);
                ShnPutU8(buf, &off, pose.sfx[k].kind);
                ShnPutU8(buf, &off, pose.sfx[k].age4);
            }
            ShnPutHeader(buf, SHNET_MSG_S_POS, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            /* On the relay one room-wide datagram replaces one per member:
             * at 60 Hz with three partners the per-member copies alone were
             * past the server's per-client packet budget. */
            if (s_backend == COOP_BACKEND_UDP)
            {
                Tp_Send(0, buf, off, 0);
                break;
            }
            Tp_Send(s_members[i].steamId, buf, off, 0);
        }
    }

    /* Co-op shared items: a local pickup goes to everyone, reliable (a dropped
     * grant means a missing key). */
    for (q = 0; q < itemOutN; q++)
    {
        for (i = 0; i < s_memberCount; i++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 4];
            int    off = SHNET_HDR_SIZE;
            if (s_members[i].steamId == Tp_SelfId())
            {
                continue;
            }
            ShnPutU8(buf, &off, itemOut[q].id);
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, itemOut[q].n);
            ShnPutHeader(buf, SHNET_MSG_S_ITEM, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(s_members[i].steamId, buf, off, 1);
        }
        SH_DBG("[SESSION] shared pickup item %d x%d to session", itemOut[q].id, itemOut[q].n);
    }

    /* Friendly-fire hits we landed, each to its one victim. */
    {
        ShSessionDmg dmg[SESSION_DMG_Q];
        int          dn, q;
        SDL_LockMutex(s_lock);
        dn = s_dmgOutCount;
        if (dn > 0) memcpy(dmg, s_dmgOut, (size_t)dn * sizeof(dmg[0]));
        s_dmgOutCount = 0;
        SDL_UnlockMutex(s_lock);
        for (q = 0; q < dn; q++)
        {
            shn_u8 buf[SHNET_HDR_SIZE + 20];
            int    off = SHNET_HDR_SIZE;
            ShnPutU32(buf, &off, (shn_u32)dmg[q].amount);
            ShnPutU32(buf, &off, (shn_u32)dmg[q].kbx);
            ShnPutU32(buf, &off, (shn_u32)dmg[q].kby);
            ShnPutU32(buf, &off, (shn_u32)dmg[q].kbz);
            ShnPutU8(buf, &off, dmg[q].reaction);
            ShnPutU8(buf, &off, 0);
            ShnPutU16(buf, &off, (shn_u16)dmg[q].yaw);
            ShnPutHeader(buf, SHNET_MSG_S_DAMAGE, (shn_u16)(off - SHNET_HDR_SIZE), 0);
            Tp_Send(dmg[q].to, buf, off, 1);
        }
    }

    ShSession_Receive(nowMs);

    for (i = 0; i < s_memberCount; i++)
    {
        if (s_members[i].steamId == Tp_SelfId())
        {
            s_members[i].linked = 1;
            s_members[i].pingMs = 0;
            continue;
        }
        if (s_members[i].linked && nowMs - s_lastSeenMs[i] > SESSION_LINK_MS)
        {
            s_members[i].linked = 0;
            s_members[i].pingMs = -1;
        }
    }

    /* THE CO-OP FLAG. It is deliberately NOT "two people are in a lobby":
     * standing in a lobby together is not standing in each other's world, and
     * blocking pause for it would be wrong. It wants the world handshake that
     * joining someone's game will perform, which does not exist yet -- so
     * nothing here turns it on, and `net coop 1` is how the suppression is
     * exercised until it does. When co-op lands, this is where it goes. */

    ShSession_Publish();
}
