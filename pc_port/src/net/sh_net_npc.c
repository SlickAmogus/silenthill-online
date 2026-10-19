/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_npc.c - the host's monsters, shared with everyone in a co-op game.
 *
 * The host runs monsters exactly as single-player does. A few times a frame
 * second it sends what each one looks like (position, the exact keyframe pair
 * and blend its AI last posed it with, health, collision); every guest shows
 * them as PUPPETS: real entries in g_SysWork.npcs whose AI is skipped and
 * whose state comes from the host instead, so they draw, light, block, show on
 * the radio and take bullets through the game's own code. A guest's hit on a
 * puppet is lifted off it before anything can react and sent to the host,
 * which applies it to the real monster the way func_8008B714 would have; the
 * host's AI then flinches, staggers or dies, and the guest sees that in the
 * next snapshot.
 *
 * Scope: every monster with a spawn row (npc->field_40): the map's spawn
 * table, including the bosses that live in it (Split Head, Floatstinger,
 * Twinfeeler), and the scripted Chara_Spawn monsters that use the same row
 * identity. Monster Cybil, the Incubus and cutscene actors stay local: their
 * map scripts write their slots directly. A boss is shown on a guest only
 * once that guest's own intro is done (the intro script drives slot 0).
 *
 * The host owns the whole map: it streams chunks and keeps collision around
 * every partner (bodyprog_80040B74.c), spawns inside a partner's ring as it
 * would its own (npc_main.c) and keeps a monster as long as any player is
 * near it, so a guest never spawns a table monster while the host is on the
 * map. Kills and the world's progress travel as the host's ovlEnemyStates
 * and event flags.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/player.h"
#include "bodyprog/collision/collision.h"

#include <string.h>

#include "sh_net.h"
#include "pc_config.h" /* Pc_LongViewActive */
#include "sh_net_npc.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_log.h"

void func_8005DC1C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType);
void func_8005DD44(e_SfxId sfxId, VECTOR3* pos, q23_8 vol, s8 pitch);

#define NPC_ROWS        32          /* field_228C / ovlEnemyStates are 32-bit masks */
#define NPC_SNAP_MS     33          /* ~30 Hz */
#define NPC_ENTRY       67          /* exactly what Npc_PutEntry writes */
#define NPC_PER_DGRAM   16
#define NPC_SNAP_HDR    13
#define NPC_SFX_MAX     8
#define NPC_SFX_ENTRY   7
#define NPC_SFX_RESEND  150
#define NPC_GONE_MS     700         /* a row missing from snapshots this long is gone */

extern int g_PcCoopGame;

/* ------------------------------------------------------------------ */
/* Role                                                                */
/* ------------------------------------------------------------------ */

static int Npc_Role(void)
{
    if (!g_PcCoopGame || !ShSession_Active() || ShSession_MemberCount() < 2 ||
        g_SavegamePtr == NULL || g_GameWork.gameState != GameState_InGame)
    {
        return PC_NETNPC_NONE;
    }
    return ShSession_IsHost() ? PC_NETNPC_HOST : PC_NETNPC_GUEST;
}

int Pc_NetNpc_Role(void)
{
    return Npc_Role();
}

static int Npc_Replicable(const s_SubCharacter* npc)
{
    return npc->model.charaId > Chara_None && npc->model.charaId <= Chara_MonsterCybil &&
           npc->model.charaId != Chara_Padlock && npc->model.charaId != Chara_MonsterCybil &&
           npc->model.charaId != Chara_Incubus && npc->field_40 >= 0 && npc->field_40 < NPC_ROWS;
}

/* Partners on this map with a live pose: world position and chunk cell.
 * Host role only; cached briefly since the chunk code asks every frame. */
static int Npc_Partners(s32* px, s32* pz, s32* cx, s32* cz, int max)
{
    static s32          c_px[SHSESSION_MAX_MEMBERS], c_pz[SHSESSION_MAX_MEMBERS];
    static s32          c_cx[SHSESSION_MAX_MEMBERS], c_cz[SHSESSION_MAX_MEMBERS];
    static int          c_n;
    static unsigned int c_ms;
    const unsigned int  now = ShNetPlat_Millis();
    int                 i, n;

    if (now - c_ms > 50u)
    {
        c_ms = now;
        c_n  = 0;
        if (Npc_Role() == PC_NETNPC_HOST)
        {
            n = ShSession_MemberCount();
            for (i = 0; i < n && c_n < SHSESSION_MAX_MEMBERS; i++)
            {
                const ShSessionMember* m = ShSession_Member(i);
                ShSessionPoseNow       pn;
                if (!m || m->steamId == ShSession_SelfId() || m->poseMs == 0 ||
                    m->mapIdx != (int)g_SavegamePtr->mapIdx || (int)(now - m->poseMs) > 3000 ||
                    !ShSession_MemberPoseNow(i, &pn))
                {
                    continue;
                }
                c_px[c_n] = pn.x;
                c_pz[c_n] = pn.z;
                c_cx[c_n] = FLOOR_TO_STEP(Q12_TO_Q8(pn.x), Q12_TO_Q8(CHUNK_CELL_SIZE));
                c_cz[c_n] = FLOOR_TO_STEP(Q12_TO_Q8(pn.z), Q12_TO_Q8(CHUNK_CELL_SIZE));
                c_n++;
            }
        }
    }
    n = c_n < max ? c_n : max;
    for (i = 0; i < n; i++)
    {
        if (px) px[i] = c_px[i];
        if (pz) pz[i] = c_pz[i];
        if (cx) cx[i] = c_cx[i];
        if (cz) cz[i] = c_cz[i];
    }
    return n;
}

int Pc_NetNpc_PartnerCells(s32* px, s32* pz, s32* cx, s32* cz, int max)
{
    return Npc_Partners(px, pz, cx, cz, max);
}

int Pc_NetNpc_ChunkNearPartner(s32 cellX, s32 cellZ, int isExterior)
{
    s32 cx[SHSESSION_MAX_MEMBERS], cz[SHSESSION_MAX_MEMBERS];
    int i, n = Npc_Partners(NULL, NULL, cx, cz, SHSESSION_MAX_MEMBERS);
    /* The same windows the host's own cell keeps: +-1 outside, the +-2 X /
     * +-1 Z interior draw window inside. */
    const s32 wx = isExterior ? 1 : 2, wz = 1;
    for (i = 0; i < n; i++)
    {
        if (cellX >= cx[i] - wx && cellX <= cx[i] + wx && cellZ >= cz[i] - wz && cellZ <= cz[i] + wz)
        {
            return 1;
        }
    }
    return 0;
}

int Pc_NetNpc_PartnerSpawnOk(s32 x, s32 z, int cond)
{
    s32 px[SHSESSION_MAX_MEMBERS], pz[SHSESSION_MAX_MEMBERS];
    int i, n = Npc_Partners(px, pz, NULL, NULL, SHSESSION_MAX_MEMBERS);
    const long long ring = Pc_LongViewActive() ? Q12(30.0f) : Q12(22.0f);
    const long long near = Q12(12.0f);
    for (i = 0; i < n; i++)
    {
        long long dx = (long long)px[i] - x, dz = (long long)pz[i] - z, d2 = dx * dx + dz * dz;
        if (d2 <= ring * ring && (!cond || d2 > near * near))
        {
            /* Only onto ground the host actually has: the chunk may still
             * be on its way in. */
            s_CollisionSurface surf;
            Collision_SurfaceGet(&surf, x, z);
            return surf.groundHeight != Q12(8.0f);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Host: what each tracked monster looks like                          */
/* ------------------------------------------------------------------ */

static u8  s_tracked[NPC_COUNT_MAX];   /* spawned by the table loop on the host */
static u8  s_trackChara[NPC_COUNT_MAX];
static u8  s_gen[NPC_COUNT_MAX];
static struct { s32 k0, k1, a; } s_pose[NPC_COUNT_MAX];
static unsigned long long s_target[NPC_COUNT_MAX];   /* 0 = the host itself */
static struct { unsigned long long peer; u16 seq; int ok; } s_hitSeen[SHSESSION_MAX_MEMBERS];

GsCOORDINATE2* g_PcNetNpcCapBones = NULL;
static int     s_capSlot          = -1;

void Pc_NetNpc_OnSpawn(int slot)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX) return;
    s_tracked[slot]    = 1;
    s_trackChara[slot] = (u8)g_SysWork.npcs[slot].model.charaId;
    s_gen[slot]++;
    s_pose[slot].k0 = s_pose[slot].k1 = s_pose[slot].a = 0;
}

/* ---- Host: monsters go after guests too ----
 * Every monster's AI is written against g_SysWork.playerWork (about 300
 * references across the monster files), so rather than teach each one about
 * other players, the AI is handed a different player: while a monster whose
 * nearest player is a guest updates, playerWork holds a PROXY standing where
 * that guest is drawn, and what the AI does to the proxy (damage, knockback,
 * the attack it landed) is lifted off afterwards and sent to the guest. The
 * proxy is rebuilt from the host's own playerWork every time, so a grab or a
 * state change the AI made to it never lingers. */
static s_PlayerWork       s_savedWork;
static u8                 s_savedDisableDamage;
static u16                s_savedGroupFlags[CHARA_GROUP_COUNT];
static s8                 s_savedNpcIdxs[CHARA_GROUP_COUNT];
static int                s_swapSlot = -1;
static unsigned long long s_swapTo;
int                       g_PcNetNpcSwapActive = 0;  /* sh_net_game.c: not our own sound */

static void Npc_PickTarget(int slot)
{
    const s_SubCharacter* npc  = &g_SysWork.npcs[slot];
    const unsigned int    now  = ShNetPlat_Millis();
    unsigned long long    best = 0, cur = s_target[slot];
    long long             bestD, curD = -1;
    int                   i, n;

    {
        long long dx = (long long)g_SysWork.playerWork.player.position.vx - npc->position.vx;
        long long dz = (long long)g_SysWork.playerWork.player.position.vz - npc->position.vz;
        bestD = (g_SysWork.playerWork.player.health > 0) ? dx * dx + dz * dz : (1LL << 52);
        if (cur == 0) curD = bestD;
    }
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        ShSessionPoseNow       pn;
        long long              dx, dz, d;
        if (!m || m->steamId == ShSession_SelfId() || m->poseMs == 0 ||
            m->mapIdx != (int)g_SavegamePtr->mapIdx || (int)(now - m->poseMs) > 1000 ||
            !(m->flags & SHNET_PF_ALIVE) || !ShSession_MemberPoseNow(i, &pn))
        {
            continue;
        }
        dx = (long long)pn.x - npc->position.vx;
        dz = (long long)pn.z - npc->position.vz;
        d  = dx * dx + dz * dz;
        if (m->steamId == cur) curD = d;
        if (d < bestD) { bestD = d; best = m->steamId; }
    }
    /* Stay on the current target unless someone is clearly closer (squared:
     * 0.49 is 70% of the distance), so a monster between two players does
     * not flicker between them every frame. */
    if (curD >= 0 && !(bestD * 100 < curD * 49))
    {
        return;
    }
    s_target[slot] = best;
}

static int Npc_SwapIn(int slot)
{
    int i, n;
    if (Npc_Role() != PC_NETNPC_HOST)
    {
        s_target[slot] = 0;
        return 0;
    }
    Npc_PickTarget(slot);
    if (s_target[slot] == 0)
    {
        return 0;
    }
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        ShSessionPoseNow       pn;
        s_SubCharacter*        px = &g_SysWork.playerWork.player;
        if (!m || m->steamId != s_target[slot] || !ShSession_MemberPoseNow(i, &pn))
        {
            continue;
        }
        s_savedWork          = g_SysWork.playerWork;
        s_savedDisableDamage = g_Player_DisableDamage;
        memcpy(s_savedGroupFlags, g_SysWork.charaGroupFlags, sizeof(s_savedGroupFlags));
        memcpy(s_savedNpcIdxs, g_SysWork.npcIdxs, sizeof(s_savedNpcIdxs));
        g_Player_DisableDamage = 0;
        px->position.vx    = pn.x;
        px->position.vy    = pn.y;
        px->position.vz    = pn.z;
        px->rotation.vy    = pn.rotY;
        px->health         = Q12(100.0f);
        px->damage.amount      = 0;
        px->damage.position.vx = 0;
        px->damage.position.vy = 0;
        px->damage.position.vz = 0;
        px->attackReceived = NO_VALUE;
        px->field_40       = NO_VALUE;
        px->collision.shapeOffsets.box.vx      = 0;
        px->collision.shapeOffsets.box.vz      = 0;
        px->collision.shapeOffsets.cylinder.vx = 0;
        px->collision.shapeOffsets.cylinder.vz = 0;
        g_SysWork.playerWork.extra.state = PlayerState_None;
        s_swapSlot           = slot;
        s_swapTo             = s_target[slot];
        g_PcNetNpcSwapActive = 1;
        return 1;
    }
    s_target[slot] = 0;
    return 0;
}

static u16 s_atkSeq;

static void Npc_SwapOut(void)
{
    const s_SubCharacter* px = &g_SysWork.playerWork.player;
    if (s_swapSlot < 0)
    {
        return;
    }
    if (px->damage.amount > 0)
    {
        /* Grab drains add damage with no attack id; they land as a flinch. */
        shn_u8 b[24];
        int    o = 0, r;
        ShnPutU8(b, &o, (shn_u8)g_SysWork.npcs[s_swapSlot].field_40);
        ShnPutU8(b, &o, (shn_u8)(px->attackReceived >= 40 && px->attackReceived < 70 ? px->attackReceived : 43));
        ShnPutU16(b, &o, ++s_atkSeq);
        ShnPutS32(b, &o, px->damage.amount);
        ShnPutS32(b, &o, px->damage.position.vx);
        ShnPutS32(b, &o, px->damage.position.vy);
        ShnPutS32(b, &o, px->damage.position.vz);
        for (r = 0; r < 2; r++)
        {
            ShSession_SendGame(SHNET_MSG_S_NPC_ATK, b, o, s_swapTo, 1);
        }
    }
    g_SysWork.playerWork   = s_savedWork;
    g_Player_DisableDamage = s_savedDisableDamage;
    memcpy(g_SysWork.charaGroupFlags, s_savedGroupFlags, sizeof(s_savedGroupFlags));
    memcpy(g_SysWork.npcIdxs, s_savedNpcIdxs, sizeof(s_savedNpcIdxs));
    g_PcNetNpcSwapActive = 0;
    s_swapSlot           = -1;
}

void Pc_NetNpc_BeginUpdate(int slot, GsCOORDINATE2* bones)
{
    if (slot >= 0 && slot < NPC_COUNT_MAX && s_tracked[slot] &&
        g_SysWork.npcs[slot].model.charaId == (e_CharaId)s_trackChara[slot] &&
        Npc_Replicable(&g_SysWork.npcs[slot]))
    {
        s_capSlot          = slot;
        g_PcNetNpcCapBones = bones;
        Npc_SwapIn(slot);
    }
}

void Pc_NetNpc_EndUpdate(void)
{
    Npc_SwapOut();
    s_capSlot          = -1;
    g_PcNetNpcCapBones = NULL;
}

void Pc_NetNpc_CapturePose(s32 k0, s32 k1, s32 alpha)
{
    if (s_capSlot >= 0)
    {
        s_pose[s_capSlot].k0 = k0;
        s_pose[s_capSlot].k1 = k1;
        s_pose[s_capSlot].a  = alpha;
    }
}

/* Sounds a tracked monster made this frame, to ride the next snapshots. */
static struct { unsigned int ms; u16 sfx; u8 row, vol, kind, seq; } s_hsfx[16];
static int s_hsfxN;
static u8  s_hsfxSeq;

void Pc_NetNpc_Sfx(const void* pos, int sfxId, int vol, int pitched)
{
    const u8* base = (const u8*)&g_SysWork.npcs[0].position;
    ptrdiff_t off  = (const u8*)pos - base;
    int       slot;

    if (off < 0 || off % (ptrdiff_t)sizeof(s_SubCharacter) != 0 || Npc_Role() != PC_NETNPC_HOST)
    {
        return;
    }
    slot = (int)(off / (ptrdiff_t)sizeof(s_SubCharacter));
    if (slot >= NPC_COUNT_MAX || !s_tracked[slot] || !Npc_Replicable(&g_SysWork.npcs[slot]))
    {
        return;
    }
    if (s_hsfxN == (int)(sizeof(s_hsfx) / sizeof(s_hsfx[0])))
    {
        memmove(&s_hsfx[0], &s_hsfx[1], sizeof(s_hsfx[0]) * (size_t)(s_hsfxN - 1));
        s_hsfxN--;
    }
    s_hsfx[s_hsfxN].ms   = ShNetPlat_Millis();
    s_hsfx[s_hsfxN].sfx  = (u16)sfxId;
    s_hsfx[s_hsfxN].row  = (u8)g_SysWork.npcs[slot].field_40;
    s_hsfx[s_hsfxN].vol  = (u8)(vol < 0 ? 0 : vol > 255 ? 255 : vol);
    s_hsfx[s_hsfxN].kind = (u8)(pitched ? 1 : 0);
    s_hsfx[s_hsfxN].seq  = ++s_hsfxSeq;
    s_hsfxN++;
}

static void Npc_PutEntry(shn_u8* b, int* o, int slot)
{
    const s_SubCharacter* n = &g_SysWork.npcs[slot];
    const int             start = *o;
    ShnPutU8(b, o, (shn_u8)n->field_40);
    ShnPutU8(b, o, s_gen[slot]);
    ShnPutU8(b, o, (shn_u8)n->model.charaId);
    ShnPutU8(b, o, (shn_u8)n->model.paletteIdx);
    ShnPutS32(b, o, n->position.vx);
    ShnPutS32(b, o, n->position.vy);
    ShnPutS32(b, o, n->position.vz);
    ShnPutS16(b, o, n->rotation.vx);
    ShnPutS16(b, o, n->rotation.vy);
    ShnPutS16(b, o, n->rotation.vz);
    ShnPutU16(b, o, (shn_u16)s_pose[slot].k0);
    ShnPutU16(b, o, (shn_u16)s_pose[slot].k1);
    ShnPutU16(b, o, (shn_u16)s_pose[slot].a);
    ShnPutS32(b, o, n->health);
    ShnPutS16(b, o, n->flags);
    ShnPutU16(b, o, n->model.anim.flags);
    ShnPutS16(b, o, n->timer_C6);
    memcpy(b + *o, &n->collision, 24);  /* box, cylinder, shape offsets: plain s16s */
    *o += 24;
    ShnPutU8(b, o, (shn_u8)n->collision.state);
    ShnPutU8(b, o, n->collision.field_E0);
    ShnPutU16(b, o, n->deathTimer);
    ShnPutU8(b, o, (shn_u8)slot); /* a boss's map script reads its slot (npcs[0]) */
    if (*o - start != NPC_ENTRY)
    {
        static int s_said;
        if (!s_said) { s_said = 1; SH_WARN("[NETNPC] snapshot entry is %d bytes, expected %d", *o - start, NPC_ENTRY); }
    }
}

static void Npc_HostSend(void)
{
    static unsigned int s_lastMs;
    static shn_u16      s_seq;
    const unsigned int  now = ShNetPlat_Millis();
    int                 slots[NPC_COUNT_MAX];
    int                 n = 0, k, sent = 0;

    if (now - s_lastMs < NPC_SNAP_MS)
    {
        return;
    }
    s_lastMs = now;

    for (k = 0; k < NPC_COUNT_MAX; k++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[k];
        if (!s_tracked[k])
        {
            continue;
        }
        /* A scripted spawn reusing the slot is not ours any more. */
        if (npc->model.charaId != (e_CharaId)s_trackChara[k] || !Npc_Replicable(npc))
        {
            s_tracked[k] = 0;
            continue;
        }
        slots[n++] = k;
    }

    /* Always at least one datagram: an empty snapshot is how guests learn
     * that the last monster is gone, and it carries the kill bits. */
    do
    {
        shn_u8 b[SHNET_MAX_PAYLOAD - SHNET_HDR_SIZE];
        int    o = 0, cnt = n - sent, i, ns = 0;
        if (cnt > NPC_PER_DGRAM) cnt = NPC_PER_DGRAM;
        ShnPutU32(b, &o, now);
        ShnPutU8(b, &o, (shn_u8)g_SavegamePtr->mapIdx);
        ShnPutU8(b, &o, (shn_u8)cnt);
        ShnPutU16(b, &o, ++s_seq);
        ShnPutU32(b, &o, (shn_u32)g_SavegamePtr->ovlEnemyStates[g_SavegamePtr->mapIdx]);
        for (i = 0; i < NPC_SFX_MAX && i < s_hsfxN; i++)
        {
            if (now - s_hsfx[s_hsfxN - 1 - i].ms > NPC_SFX_RESEND) break;
            ns++;
        }
        if (sent > 0) ns = 0; /* sounds ride the first datagram only */
        ShnPutU8(b, &o, (shn_u8)ns);
        for (i = 0; i < cnt; i++)
        {
            Npc_PutEntry(b, &o, slots[sent + i]);
        }
        for (i = ns - 1; i >= 0; i--) /* oldest first */
        {
            int e = s_hsfxN - 1 - i;
            ShnPutU8(b, &o, s_hsfx[e].row);
            ShnPutU8(b, &o, s_hsfx[e].seq);
            ShnPutU16(b, &o, s_hsfx[e].sfx);
            ShnPutU8(b, &o, s_hsfx[e].vol);
            ShnPutU8(b, &o, s_hsfx[e].kind);
            ShnPutU8(b, &o, (shn_u8)((now - s_hsfx[e].ms) / 4u));
        }
        if (!ShSession_SendGame(SHNET_MSG_S_NPC_SNAP, b, o, 0, 0))
        {
            static int s_dropSaid;
            if (!s_dropSaid) { s_dropSaid = 1; SH_WARN("[NETNPC] snapshot queue full; dropping"); }
        }
        /* A few lines per session, so a log shows whether monsters are
         * being shared at all. */
        {
            static int s_logged, s_lastN = -1;
            if (n != s_lastN && s_logged < 12)
            {
                s_logged++;
                s_lastN = n;
                SH_DBG("[NETNPC] host: sharing %d monster(s) on map %d", n, (int)g_SavegamePtr->mapIdx);
            }
        }
        sent += cnt;
    } while (sent < n);
}

/* Host: a guest hit one of our monsters. */

static void Npc_HostApplyHit(unsigned long long from, const shn_u8* p, int len)
{
    int     o = 0, k, row, gen, i, slot = -1;
    u16     seq;
    s32     amount, kbx, kby, kbz;
    s8      atk;

    if (len < 22) return;
    row    = ShnGetU8(p, &o);
    gen    = ShnGetU8(p, &o);
    seq    = ShnGetU16(p, &o);
    amount = ShnGetS32(p, &o);
    kbx    = ShnGetS32(p, &o);
    kby    = ShnGetS32(p, &o);
    kbz    = ShnGetS32(p, &o);
    atk    = (s8)ShnGetU8(p, &o);

    /* Each hit is sent twice (the relay has no reliable channel). */
    for (i = 0; i < SHSESSION_MAX_MEMBERS && s_hitSeen[i].ok && s_hitSeen[i].peer != from; i++) {}
    if (i == SHSESSION_MAX_MEMBERS) i = 0;
    if (s_hitSeen[i].ok && s_hitSeen[i].peer == from && (s16)(seq - s_hitSeen[i].seq) <= 0) return;
    s_hitSeen[i].ok   = 1;
    s_hitSeen[i].peer = from;
    s_hitSeen[i].seq  = seq;

    /* A weapon hit is at most a stomp on a downed monster (x4 unflagged, see
     * func_8008B714); knockback is a stagger, not a teleport. Player weapon
     * attacks only (0..69). */
    if (amount <= 0 || amount > Q12(5000.0f) || atk < 0 || atk >= 70) return;
    if (kbx > Q12(2.0f) || kbx < -Q12(2.0f) || kby > Q12(2.0f) || kby < -Q12(2.0f) ||
        kbz > Q12(2.0f) || kbz < -Q12(2.0f)) return;

    for (k = 0; k < NPC_COUNT_MAX; k++)
    {
        if (s_tracked[k] && g_SysWork.npcs[k].field_40 == row && s_gen[k] == gen &&
            Npc_Replicable(&g_SysWork.npcs[k]))
        {
            slot = k;
            break;
        }
    }
    if (slot < 0) return;
    {
        static int      s_logged;
        s_SubCharacter* n = &g_SysWork.npcs[slot];
        if (s_logged < 12)
        {
            s_logged++;
            SH_DBG("[NETNPC] host: a partner hit row %d (chara %d) with attack %d for %d.%02d",
                   row, (int)n->model.charaId, atk, amount >> 12, ((amount & 0xFFF) * 100) >> 12);
        }
        n->damage.amount      += amount;
        n->damage.position.vx += kbx;
        n->damage.position.vy += kby;
        n->damage.position.vz += kbz;
        n->attackReceived      = atk;
    }
}

/* ------------------------------------------------------------------ */
/* Guest: puppets                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    s32 x, y, z;
    s16 rx, ry, rz;
    u16 k0, k1, a;
    s32 health;
    s16 flags;
    u16 animFlags;
    s16 timer;
    u8  col[24];
    u8  colState, colE0;
    u16 deathTimer;
} NpcSample;

typedef struct
{
    int          used;
    int          slot;
    u8           gen;
    u8           charaId;
    u8           palette;
    NpcSample    prev, cur;
    unsigned int prevMs, curMs;
    unsigned int seenMs;
} Puppet;

static Puppet s_pup[NPC_ROWS];
static s8     s_slotRow[NPC_COUNT_MAX];
static int    s_pupInit;
static u16    s_hitSeq;
static u8     s_lastSfxSeq;
static int    s_sfxSeqOk;

static void Npc_PupInit(void)
{
    int k;
    if (s_pupInit) return;
    s_pupInit = 1;
    for (k = 0; k < NPC_COUNT_MAX; k++) s_slotRow[k] = -1;
}

int Pc_NetNpc_IsPuppet(int slot)
{
    int row;
    if (!s_pupInit || slot < 0 || slot >= NPC_COUNT_MAX || (row = s_slotRow[slot]) < 0) return 0;
    /* The slot must still be the puppet we made, not something that reused
     * it after a wipe we did not see. */
    return s_pup[row].used && s_pup[row].slot == slot &&
           g_SysWork.npcs[slot].model.charaId == (e_CharaId)s_pup[row].charaId &&
           g_SysWork.npcs[slot].field_40 == row && HAS_FLAG(&g_SysWork.npcFlags, slot);
}

static u16 s_atkSeqIn;
static int s_atkSeqInOk;

/* A room or map load wipes g_SysWork.npcs (GameBoot_NpcClear); every slot
 * reference here goes with it, and so do the message counters, since the
 * other side may have restarted too. */
void Pc_NetNpc_Reset(void)
{
    int k;
    for (k = 0; k < NPC_COUNT_MAX; k++)
    {
        s_slotRow[k] = -1;
        s_tracked[k] = 0;
        s_target[k]  = 0;
    }
    memset(s_pup, 0, sizeof(s_pup));
    memset(s_hitSeen, 0, sizeof(s_hitSeen));
    s_pupInit    = 1;
    s_sfxSeqOk   = 0;
    s_atkSeqInOk = 0;
    s_hsfxN      = 0;
}

void Pc_NetNpc_OnFree(int slot)
{
    if (slot >= 0 && slot < NPC_COUNT_MAX)
    {
        s_tracked[slot] = 0;
        s_target[slot]  = 0;
    }
}

static void Npc_PupFree(int row)
{
    Puppet* p = &s_pup[row];
    if (!p->used) return;
    {
        static int s_logged;
        if (s_logged < 12)
        {
            s_logged++;
            SH_DBG("[NETNPC] guest: puppet row %d released (%s)", row,
                   (unsigned)(ShNetPlat_Millis() - p->seenMs) > NPC_GONE_MS ? "gone from snapshots" : "replaced or reset");
        }
    }
    if (p->slot >= 0 && p->slot < NPC_COUNT_MAX && s_slotRow[p->slot] == row)
    {
        s_SubCharacter* n = &g_SysWork.npcs[p->slot];
        if (n->model.charaId == (e_CharaId)p->charaId)
        {
            n->model.charaId = Chara_None;
            SysWork_NpcFlagClear(p->slot);
        }
        s_slotRow[p->slot] = -1;
    }
    CLEAR_FLAG(g_SysWork.field_228C, row);
    p->used = 0;
}

static int Npc_PupAlloc(int row, int charaId, int palette, int gen, int wantSlot)
{
    int k, slot = -1;

    if ((s8)g_CharaAnimDataIdxs[charaId] == (s8)0xFF)
    {
        return 0; /* model not resident here yet; next snapshot */
    }
    /* Our own copy of this row (spawned while the host was away) gives way --
     * a table spawn of the same monster only, and never one holding us. */
    for (k = 0; k < NPC_COUNT_MAX; k++)
    {
        extern u8       g_PcNpcDebugSpawned[NPC_COUNT_MAX];
        s_SubCharacter* n = &g_SysWork.npcs[k];
        int             g;
        if (s_slotRow[k] >= 0 || !Npc_Replicable(n) || n->field_40 != row || n->model.charaId != (e_CharaId)charaId ||
            !HAS_FLAG(&g_SysWork.npcFlags, k) || g_PcNpcDebugSpawned[k])
        {
            continue;
        }
        for (g = 0; g < CHARA_GROUP_COUNT; g++)
        {
            if (g_SysWork.npcIdxs[g] == k) return 0;
        }
        n->model.charaId = Chara_None;
        SysWork_NpcFlagClear(k);
    }
    /* The host's slot when it is free here: a boss's map script reads
     * npcs[0] (Split Head's BGM, the intro helpers), so the puppet must sit
     * where the script looks. */
    if (wantSlot >= 0 && wantSlot < NPC_COUNT_MAX && !HAS_FLAG(&g_SysWork.npcFlags, wantSlot) &&
        g_SysWork.npcs[wantSlot].model.charaId == Chara_None)
    {
        slot = wantSlot;
    }
    for (k = 0; slot < 0 && k < NPC_COUNT_MAX; k++)
    {
        if (!HAS_FLAG(&g_SysWork.npcFlags, k) && g_SysWork.npcs[k].model.charaId == Chara_None)
        {
            slot = k;
        }
    }
    if (slot < 0) return 0;

    memset(&g_SysWork.npcs[slot], 0, sizeof(s_SubCharacter));
    {
        extern u8 g_PcNpcDebugSpawned[NPC_COUNT_MAX];
        g_PcNpcDebugSpawned[slot] = 1; /* never writes this row's kill bit here */
    }
    g_SysWork.npcs[slot].model.charaId    = (e_CharaId)charaId;
    g_SysWork.npcs[slot].model.paletteIdx = (u8)palette;
    g_SysWork.npcs[slot].field_40         = (s8)row;
    g_SysWork.npcs[slot].attackReceived   = NO_VALUE;
    g_SysWork.npcs[slot].model.anim.flags = AnimFlag_Visible | AnimFlag_Unlocked;
    SET_FLAG(&g_SysWork.npcFlags, slot);
    SET_FLAG(g_SysWork.field_228C, row);
    s_slotRow[slot]    = (s8)row;
    {
        static int s_logged;
        if (s_logged < 12)
        {
            s_logged++;
            SH_DBG("[NETNPC] guest: puppet for the host's row %d (chara %d) in slot %d", row, charaId, slot);
        }
    }
    s_pup[row].used    = 1;
    s_pup[row].slot    = slot;
    s_pup[row].gen     = (u8)gen;
    s_pup[row].charaId = (u8)charaId;
    s_pup[row].palette = (u8)palette;
    return 1;
}

static void Npc_GuestSnapshot(const shn_u8* p, int len)
{
    const unsigned int now = ShNetPlat_Millis();
    int                o = 0, i, cnt, ns, map;
    shn_u32            states;

    if (len < NPC_SNAP_HDR) return;
    (void)ShnGetU32(p, &o);
    map    = ShnGetU8(p, &o);
    cnt    = ShnGetU8(p, &o);
    (void)ShnGetU16(p, &o);
    states = ShnGetU32(p, &o);
    ns     = ShnGetU8(p, &o);
    if (map != (int)g_SavegamePtr->mapIdx || map >= 45) return;
    if (cnt > NPC_PER_DGRAM || ns > NPC_SFX_MAX) return;
    if (NPC_SNAP_HDR + cnt * NPC_ENTRY + ns * NPC_SFX_ENTRY > len) return;

    /* The host killed it: it stays dead here too. */
    g_SavegamePtr->ovlEnemyStates[map] &= (s32)states;

    for (i = 0; i < cnt; i++)
    {
        int       row     = ShnGetU8(p, &o);
        int       gen     = ShnGetU8(p, &o);
        int       charaId = ShnGetU8(p, &o);
        int       palette = ShnGetU8(p, &o);
        int       hostSlot;
        NpcSample s;
        Puppet*   pp;

        s.x  = ShnGetS32(p, &o);
        s.y  = ShnGetS32(p, &o);
        s.z  = ShnGetS32(p, &o);
        s.rx = ShnGetS16(p, &o);
        s.ry = ShnGetS16(p, &o);
        s.rz = ShnGetS16(p, &o);
        s.k0 = ShnGetU16(p, &o);
        s.k1 = ShnGetU16(p, &o);
        s.a  = ShnGetU16(p, &o);
        s.health    = ShnGetS32(p, &o);
        s.flags     = ShnGetS16(p, &o);
        s.animFlags = ShnGetU16(p, &o);
        s.timer     = ShnGetS16(p, &o);
        memcpy(s.col, p + o, 24);
        o += 24;
        s.colState   = ShnGetU8(p, &o);
        s.colE0      = ShnGetU8(p, &o);
        s.deathTimer = ShnGetU16(p, &o);
        hostSlot     = ShnGetU8(p, &o);

        /* What a peer can make us do with this: pick a resident model, place
         * it inside the world, pose it from that model's own keyframes. */
        if (row >= NPC_ROWS || charaId <= Chara_None || charaId > Chara_MonsterCybil || charaId == Chara_Padlock ||
            charaId == Chara_MonsterCybil || charaId == Chara_Incubus)
            continue;
        /* A boss shows only once this machine's own intro is over: until
         * then the map script drives slot 0 itself (Twinfeeler's intro even
         * swaps the update function), and a puppet there would be fought
         * over. Each boss's intro leaves a flag the fight code reads. */
        if (charaId == Chara_SplitHead || charaId == Chara_Floatstinger || charaId == Chara_Twinfeeler)
        {
            int ok = g_SysWork.sysState == SysState_Gameplay && !(g_SysWork.sysFlags & SysFlag_CutsceneActive);
            if (charaId == Chara_Floatstinger && !Savegame_EventFlagGet(EventFlag_349)) ok = 0;
            if (charaId == Chara_Twinfeeler   && !Savegame_EventFlagGet(EventFlag_324)) ok = 0;
            if (!ok)
            {
                if (s_pup[row].used) Npc_PupFree(row);
                continue;
            }
        }
        if (s.x > SHNET_COORD_MAX || s.x < -SHNET_COORD_MAX || s.y > SHNET_COORD_MAX || s.y < -SHNET_COORD_MAX ||
            s.z > SHNET_COORD_MAX || s.z < -SHNET_COORD_MAX)
            continue;
        if (s.a > 4096) s.a = 4096;
        if (s.colState < 0 || s.colState > 15) s.colState = 0;

        pp = &s_pup[row];
        if (pp->used && (pp->gen != gen || pp->charaId != charaId ||
                         g_SysWork.npcs[pp->slot].model.charaId != (e_CharaId)charaId))
        {
            Npc_PupFree(row);
        }
        if (!pp->used)
        {
            if (!Npc_PupAlloc(row, charaId, palette, gen, hostSlot)) continue;
            pp->prev = pp->cur = s;
            pp->prevMs = pp->curMs = now;
        }
        else
        {
            pp->prev   = pp->cur;
            pp->prevMs = pp->curMs;
            pp->cur    = s;
            pp->curMs  = now;
        }
        pp->seenMs = now;
    }

    for (i = 0; i < ns; i++)
    {
        int       row  = ShnGetU8(p, &o);
        int       seq  = ShnGetU8(p, &o);
        int       sfx  = ShnGetU16(p, &o);
        int       vol  = ShnGetU8(p, &o);
        int       kind = ShnGetU8(p, &o);
        int       ahead;
        VECTOR3   at;
        (void)ShnGetU8(p, &o);
        ahead = (int)(u8)(seq - s_lastSfxSeq);
        if (s_sfxSeqOk && (ahead == 0 || ahead >= 128)) continue;
        s_sfxSeqOk   = 1;
        s_lastSfxSeq = (u8)seq;
        if (row >= NPC_ROWS || !s_pup[row].used) continue;
        if (sfx <= Sfx_Base || sfx >= Sfx_Base + 420) continue;
        at.vx = g_SysWork.npcs[s_pup[row].slot].position.vx;
        at.vy = g_SysWork.npcs[s_pup[row].slot].position.vy;
        at.vz = g_SysWork.npcs[s_pup[row].slot].position.vz;
        if (kind & 1) func_8005DD44((e_SfxId)sfx, &at, (q23_8)vol, 0);
        else          func_8005DC1C((e_SfxId)sfx, &at, (q23_8)vol, 0);
    }
}

/* Host: a monster is as far away as the NEAREST player, so one chasing a
 * guest is not dropped for being far from the host. Same metric as the
 * despawn test in Game_NpcUpdate. */
u32 Pc_NetNpc_NearestPlayerD2(const s_SubCharacter* npc, u32 d2)
{
    const unsigned int now = ShNetPlat_Millis();
    int                i, n;
    if (Npc_Role() != PC_NETNPC_HOST) return d2;
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        u32                    d;
        if (!m || m->steamId == ShSession_SelfId() || m->poseMs == 0 ||
            m->mapIdx != (int)g_SavegamePtr->mapIdx || (int)(now - m->poseMs) > 3000)
        {
            continue;
        }
        d = Q12_SQUARE_PRECISE(Q12_TO_Q6(npc->position.vx) - Q12_TO_Q6(m->x)) +
            Q12_SQUARE_PRECISE(Q12_TO_Q6(npc->position.vz) - Q12_TO_Q6(m->z));
        if (d < d2) d2 = d;
    }
    return d2;
}

int Pc_NetNpc_GuestSkipRow(int row, s32 spawnX, s32 spawnZ)
{
    int i, n;
    (void)row;
    if (Npc_Role() != PC_NETNPC_GUEST) return 0;
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == ShSession_HostId() && m->poseMs != 0 &&
            m->mapIdx == (int)g_SavegamePtr->mapIdx)
        {
            /* The host spawns inside our ring as it would its own and keeps
             * collision around us, so every table monster on this map is
             * the host's while the host is here. */
            (void)spawnX; (void)spawnZ;
            return (int)(ShNetPlat_Millis() - m->poseMs) <= 3000;
        }
    }
    return 0;
}

static s16 Npc_LerpAngle(s16 a, s16 b, int t)
{
    int d = ((int)(u16)b - (int)(u16)a) & 0xFFF;
    if (d >= 2048) d -= 4096;
    return (s16)(((int)a + ((d * t) >> 12)) & 0xFFF);
}

void Pc_NetNpc_PuppetTick(s_SubCharacter* npc, int slot, s_AnmHeader* hdr, GsCOORDINATE2* bones)
{
    const unsigned int now = ShNetPlat_Millis();
    int                row = s_slotRow[slot];
    Puppet*            pp;
    const NpcSample*   c;
    int                t = 4096;

    if (row < 0 || !s_pup[row].used) return;
    pp = &s_pup[row];
    c  = &pp->cur;

    /* One snapshot interval behind the newest, so the body walks between
     * two known samples instead of waiting on the next one. */
    {
        unsigned int span = pp->curMs - pp->prevMs;
        unsigned int age  = now - pp->curMs;
        if (span > 0 && span < 400u)
        {
            t = (int)((age * 4096u) / span);
            if (t > 4096) t = 4096;
        }
    }
    npc->position.vx = pp->prev.x + (int)(((long long)(c->x - pp->prev.x) * t) >> 12);
    npc->position.vy = pp->prev.y + (int)(((long long)(c->y - pp->prev.y) * t) >> 12);
    npc->position.vz = pp->prev.z + (int)(((long long)(c->z - pp->prev.z) * t) >> 12);
    npc->rotation.vx = c->rx;
    npc->rotation.vy = Npc_LerpAngle(pp->prev.ry, c->ry, t);
    npc->rotation.vz = c->rz;
    npc->health      = c->health;
    npc->flags       = c->flags;
    npc->timer_C6    = c->timer;
    npc->deathTimer  = c->deathTimer;
    memcpy(&npc->collision, c->col, 24);
    npc->collision.state    = (s8)c->colState;
    npc->collision.field_E0 = c->colE0;
    npc->model.paletteIdx   = pp->palette;
    /* Visibility stays ours: it is the guest's own fog and camera distance. */
    npc->model.anim.flags = (u16)((c->animFlags & ~AnimFlag_Visible) |
                                  (npc->model.anim.flags & AnimFlag_Visible) | AnimFlag_Unlocked);

    /* The host's pose. A pair that did not change between samples blends
     * its alpha; anything else shows the newer sample. */
    if (hdr != NULL)
    {
        int k0 = c->k0, k1 = c->k1, a = c->a;
        int maxKf = (hdr->keyframeCount > 0) ? (int)hdr->keyframeCount - 1 : 0;
        if (pp->prev.k0 == c->k0 && pp->prev.k1 == c->k1)
        {
            a = pp->prev.a + (((c->a - pp->prev.a) * t) >> 12);
        }
        if (k0 > maxKf) k0 = maxKf;
        if (k1 > maxKf) k1 = maxKf;
        Anim_BoneUpdate(hdr, bones, k0, k1, a);
        Math_RotMatrixZxyNegGte(&npc->rotation, &bones->coord);
        bones->coord.t[0] = Q12_TO_Q8(npc->position.vx);
        bones->coord.t[1] = Q12_TO_Q8(npc->position.vy);
        bones->coord.t[2] = Q12_TO_Q8(npc->position.vz);
        bones->flg = 0;
    }

    /* Our hit on it, lifted off before anything here reacts and sent to the
     * host, whose AI owns the reaction. */
    if (npc->damage.amount > 0 && npc->attackReceived >= 0)
    {
        shn_u8 b[24];
        int    o = 0, r;
        ShnPutU8(b, &o, (shn_u8)row);
        ShnPutU8(b, &o, pp->gen);
        ShnPutU16(b, &o, ++s_hitSeq);
        ShnPutS32(b, &o, npc->damage.amount);
        ShnPutS32(b, &o, npc->damage.position.vx);
        ShnPutS32(b, &o, npc->damage.position.vy);
        ShnPutS32(b, &o, npc->damage.position.vz);
        ShnPutU8(b, &o, (shn_u8)npc->attackReceived);
        ShnPutU8(b, &o, 0);
        for (r = 0; r < 2; r++)
        {
            ShSession_SendGame(SHNET_MSG_S_NPC_HIT, b, o, ShSession_HostId(), 1);
        }
    }
    npc->damage.amount      = 0;
    npc->damage.position.vx = 0;
    npc->damage.position.vy = 0;
    npc->damage.position.vz = 0;
    npc->attackReceived     = NO_VALUE;
}

/* Guest: a host monster hit US. Applied the way func_8008B714 writes a
 * monster's hit, with the attacker being our puppet of it. A grab needs the
 * live monster on this machine to hold and release us, so grabs (and the
 * instant kill) land as a hard flinch instead. */
static void Npc_GuestApplyAttack(const shn_u8* p, int len)
{
    static unsigned int s_winMs;
    static int          s_winN;
    s_SubCharacter*     me = &g_SysWork.playerWork.player;
    const unsigned int  now = ShNetPlat_Millis();
    int                 o = 0, row, atk;
    u16                 seq;
    s32                 amount, kbx, kby, kbz;

    if (len < 20) return;
    row    = ShnGetU8(p, &o);
    atk    = ShnGetU8(p, &o);
    seq    = ShnGetU16(p, &o);
    amount = ShnGetS32(p, &o);
    kbx    = ShnGetS32(p, &o);
    kby    = ShnGetS32(p, &o);
    kbz    = ShnGetS32(p, &o);

    if (s_atkSeqInOk && (s16)(seq - s_atkSeqIn) <= 0) return;
    s_atkSeqInOk = 1;
    s_atkSeqIn   = seq;
    if (now - s_winMs >= 1000u) { s_winMs = now; s_winN = 0; }
    if (++s_winN > 15) return;

    if (row >= NPC_ROWS || atk < 40 || atk >= 70 || amount <= 0 || amount > Q12(100.0f)) return;
    if (g_SysWork.sysState != SysState_Gameplay || me->health <= Q12(0.0f) || g_Player_DisableDamage) return;
    if (atk == 45 || atk == 47 || atk == 49 || atk == 54 || atk == 56 || atk == 66) atk = 43;
#define NPC_CLAMP_KB(v) ((v) > Q12(1.0f) ? Q12(1.0f) : (v) < -Q12(1.0f) ? -Q12(1.0f) : (v))
    me->damage.amount      += amount;
    me->damage.position.vx += NPC_CLAMP_KB(kbx);
    me->damage.position.vy += NPC_CLAMP_KB(kby);
    me->damage.position.vz += NPC_CLAMP_KB(kbz);
#undef NPC_CLAMP_KB
    me->attackReceived      = (s8)atk;
    me->field_40            = (s_pup[row].used) ? (s8)s_pup[row].slot : NO_VALUE;
    {
        static int s_logged;
        if (s_logged < 12)
        {
            s_logged++;
            SH_DBG("[NETNPC] guest: the host's monster (row %d) hit me with attack %d for %d.%02d",
                   row, atk, amount >> 12, ((amount & 0xFFF) * 100) >> 12);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The world's progress                                                */
/* ------------------------------------------------------------------ */

#define WORLD_FL_MS     500
#define EVENT_FLAG_WORDS 52
#define OVL_STATE_WORDS  45

/* Event flags a guest must earn itself: the boss intros, whose fight code
 * reads them the moment they are set (floatstinger.c, map4_s03/05). */
static int Npc_FlagLocal(int idx)
{
    return idx == EventFlag_324 || idx == EventFlag_348 || idx == EventFlag_349;
}

static void Npc_HostSendWorldFlags(void)
{
    static unsigned int s_lastMs;
    const unsigned int  now = ShNetPlat_Millis();
    shn_u8              b[8 + 4 * (EVENT_FLAG_WORDS + OVL_STATE_WORDS)];
    int                 o = 0, i;

    if (now - s_lastMs < WORLD_FL_MS) return;
    s_lastMs = now;
    ShnPutU8(b, &o, (shn_u8)g_SavegamePtr->mapIdx);
    ShnPutU8(b, &o, EVENT_FLAG_WORDS);
    ShnPutU8(b, &o, OVL_STATE_WORDS);
    ShnPutU8(b, &o, 0);
    for (i = 0; i < EVENT_FLAG_WORDS; i++) ShnPutU32(b, &o, g_SavegamePtr->eventFlags[i]);
    for (i = 0; i < OVL_STATE_WORDS; i++)  ShnPutU32(b, &o, (shn_u32)g_SavegamePtr->ovlEnemyStates[i]);
    ShSession_SendGame(SHNET_MSG_S_WORLD_FL, b, o, 0, 0);
}

static void Npc_GuestWorldFlags(const shn_u8* p, int len)
{
    int o = 0, i, ne, no;
    if (len < 4) return;
    (void)ShnGetU8(p, &o);
    ne = ShnGetU8(p, &o);
    no = ShnGetU8(p, &o);
    (void)ShnGetU8(p, &o);
    if (ne > EVENT_FLAG_WORDS || no > OVL_STATE_WORDS || 4 + 4 * (ne + no) > len) return;
    /* Only in plain gameplay: a flag landing mid-cutscene or mid-menu
     * would change what that script does next. */
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay ||
        (g_SysWork.sysFlags & SysFlag_CutsceneActive))
    {
        return;
    }
    for (i = 0; i < ne; i++)
    {
        shn_u32 w = ShnGetU32(p, &o), add, bit;
        add = w & ~g_SavegamePtr->eventFlags[i];
        for (bit = 0; add && bit < 32; bit++)
        {
            if ((add & (1u << bit)) && Npc_FlagLocal(i * 32 + (int)bit)) add &= ~(1u << bit);
        }
        g_SavegamePtr->eventFlags[i] |= add;
    }
    /* A kill clears a bit; the host's word is the truth for every map. */
    for (i = 0; i < no; i++)
    {
        g_SavegamePtr->ovlEnemyStates[i] &= (s32)ShnGetU32(p, &o);
    }
}

void Pc_NetNpc_PickupTaken(int eventFlagIdx)
{
    shn_u8 b[4];
    int    o = 0;
    if (!g_PcCoopGame || !ShSession_Active() || eventFlagIdx < 0 || eventFlagIdx >= EVENT_FLAG_WORDS * 32) return;
    ShnPutU16(b, &o, (shn_u16)eventFlagIdx);
    ShnPutU16(b, &o, 0);
    ShSession_SendGame(SHNET_MSG_S_FLAG_SET, b, o, 0, 1);
}

static void Npc_FlagSetIn(const shn_u8* p, int len)
{
    int o = 0, idx;
    if (len < 2) return;
    idx = ShnGetU16(p, &o);
    if (idx < 0 || idx >= EVENT_FLAG_WORDS * 32 || Npc_FlagLocal(idx)) return;
    Savegame_EventFlagSet(idx);
}

/* ------------------------------------------------------------------ */
/* Per frame                                                           */
/* ------------------------------------------------------------------ */

void Pc_NetNpc_GameTick(void)
{
    const int role = Npc_Role();
    shn_u8    buf[SHNET_MAX_PAYLOAD];
    int       type, len, r;
    unsigned long long from;

    Npc_PupInit();

    while ((len = ShSession_TakeGame(&type, &from, buf, (int)sizeof(buf))) >= 0)
    {
        if (type == SHNET_MSG_S_NPC_SNAP && role == PC_NETNPC_GUEST)
        {
            Npc_GuestSnapshot(buf, len);
        }
        else if (type == SHNET_MSG_S_NPC_HIT && role == PC_NETNPC_HOST)
        {
            Npc_HostApplyHit(from, buf, len);
        }
        else if (type == SHNET_MSG_S_NPC_ATK && role == PC_NETNPC_GUEST)
        {
            Npc_GuestApplyAttack(buf, len);
        }
        else if (type == SHNET_MSG_S_WORLD_FL && role == PC_NETNPC_GUEST)
        {
            Npc_GuestWorldFlags(buf, len);
        }
        else if (type == SHNET_MSG_S_FLAG_SET && role != PC_NETNPC_NONE)
        {
            Npc_FlagSetIn(buf, len);
        }
    }

    if (role == PC_NETNPC_HOST)
    {
        Npc_HostSend();
        Npc_HostSendWorldFlags();
    }

    /* Puppets outlive nothing: gone from the snapshots, out of the session,
     * or wiped by a room load, they are released. */
    for (r = 0; r < NPC_ROWS; r++)
    {
        Puppet* pp = &s_pup[r];
        if (!pp->used) continue;
        if (role != PC_NETNPC_GUEST || ShNetPlat_Millis() - pp->seenMs > NPC_GONE_MS ||
            g_SysWork.npcs[pp->slot].model.charaId != (e_CharaId)pp->charaId)
        {
            Npc_PupFree(r);
        }
    }
}
