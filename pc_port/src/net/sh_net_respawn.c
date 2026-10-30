/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_respawn.c - co-op respawn. Design in sh_net_respawn.h.
 *
 * Phases:  ALIVE --death anim ends--> HOLD --delay, a safe partner--> BLINK --1.5 s--> SETTLE --> ALIVE
 *
 * The player comes back next to a living partner who is not in combat (no
 * monster on them, no hit taken for a few seconds, and that for RS_CLEAR_MS,
 * so a partner who ran through a door is through it). Until there is one the
 * body waits ("Waiting to respawn"). A partner on another map is reached by
 * a reboot into it. When nobody is left alive the host gets the game's own
 * game over and loads a save; the guests wait for that and start from it.
 *
 * HOLD/BLINK: the engine's death handlers call Pc_Respawn_HoldDeath() where they
 * would enter SysState_GameOver and return instead, so the player stays in the
 * death state with the animation locked on its last keyframe (the vanilla
 * corpse pose). Monsters ignore a player whose health is <= 0, damage is
 * cleared in the death state, BGM ducks like it does for a vanilla death.
 *
 * Respawn uses the engine's own placement and recovery steps, in the order a
 * door transition and a cutscene end use them: position + facing, camera yaw,
 * func_8007E9C4 (state-machine reset), Game_MapRoomIdxUpdate, then the map's
 * playerControlUnfreeze(true) which is the game's "stand idle, controllable"
 * routine, a camera warp, and a ground probe that is retried for a few frames
 * because a far jump's chunk may stream in a frame late.
 */
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/model.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/events/bgm.h"   /* Game_MapRoomIdxUpdate */
#include "bodyprog/view/vc_util.h" /* vcSetCameraUseWarp */
#include "bodyprog/text/text_draw.h" /* the guest's "Waiting for the host" */
#include "pc_config.h"
#include "pc_coop_menu.h"          /* g_PcCoopGame */
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_net_respawn.h"
#include "sh_log.h"

#define RS_BLINK_SECONDS 1.5f
#define RS_BLINK_PERIOD  Q12(0.06f)  /* visibility flips every 60 ms */
#define RS_GRACE         Q12(2.0f)   /* post-respawn invulnerability */
#define RS_SETTLE_FRAMES 90          /* ground probe retries after a jump */
#define RS_POSE_STALE_MS 3000
#define RS_NO_FLOOR      Q12(8.0f)   /* Collision_SurfaceGet's "no floor here" */
#define RS_CLEAR_MS      2000        /* a partner is safe this long after their last combat */
#define RS_ALIVE_MS      20000       /* a pose this old no longer proves a partner alive (a load screen sends none) */

enum { RS_ALIVE = 0, RS_HOLD, RS_BLINK, RS_SETTLE };

static int    s_phase;
static q19_12 s_elapsed;      /* game time in the current phase */
static int    s_settled;
static int    s_settleFrames;
static q19_12 s_graceLeft;

/* Where the player entered the current room: the fallback spawn. */
static int     s_entryValid;
static int     s_entryMap, s_entryRoom;
static VECTOR3 s_entryPos;
static short   s_entryRot;
static int     s_lastMap = -1, s_lastRoom = -1;

/* Guest: the host's game over has begun (SHNET_CTRL_GAMEOVER); our held death
 * goes to the game's own game over too. */
static int     s_hostGameOver;

/* What the dead player is waiting for, for the status line. */
static int     s_waitWhy;  /* 0 none, 1 everyone in combat, 2 nobody alive */

/* When each partner was last seen in combat (by their pose flag). */
static struct { unsigned long long id; unsigned int ms; } s_combat[SHSESSION_MAX_MEMBERS];

static void NoteCombat(const ShSessionMember* m, unsigned int now)
{
    int i, free = -1;
    for (i = 0; i < SHSESSION_MAX_MEMBERS; i++)
    {
        if (s_combat[i].id == m->steamId) break;
        if (s_combat[i].id == 0 && free < 0) free = i;
    }
    if (i == SHSESSION_MAX_MEMBERS)
    {
        if (free < 0) return;
        i = free;
        s_combat[i].id = m->steamId;
        s_combat[i].ms = 0;
    }
    if (m->flags & SHNET_PF_COMBAT) s_combat[i].ms = now;
}

static int CombatClear(const ShSessionMember* m, unsigned int now)
{
    int i;
    if (m->flags & SHNET_PF_COMBAT) return 0;
    for (i = 0; i < SHSESSION_MAX_MEMBERS; i++)
    {
        if (s_combat[i].id == m->steamId)
        {
            return s_combat[i].ms == 0 || (int)(now - s_combat[i].ms) >= RS_CLEAR_MS;
        }
    }
    return 1;
}

static int PartnerAlive(const ShSessionMember* m, unsigned int now)
{
    return m != NULL && m->steamId != ShSession_SelfId() && m->poseMs != 0 &&
           (m->flags & SHNET_PF_ALIVE) && (int)(now - m->poseMs) <= RS_ALIVE_MS;
}

static int AnyoneElseAlive(void)
{
    const unsigned int now = ShNetPlat_Millis();
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        if (PartnerAlive(ShSession_Member(i), now)) return 1;
    }
    return 0;
}

/* The host's death with nobody else alive is the game's own game over: the
 * host loads a save and everyone starts from it. */
static int HostGameOver(void)
{
    return ShSession_IsHost() && !AnyoneElseAlive();
}

static q19_12 SecondsQ12(float s)
{
    return (q19_12)(s * 4096.0f);
}

static int InCoop(void)
{
    return g_PcCoopGame && ShSession_Active();
}

/* The world exists only inside GameState_InGame. The load screen
 * (GameState_LoadMapScreen) already runs with sysState == SysState_Gameplay
 * and a savegame while g_Map's chunk table is being torn down and rebuilt,
 * and a collision probe there dereferences stale chunk headers. InGame's own
 * update (dispatched before this tick each frame) probes collision in its
 * first step, so once it reports InGame the map is safe to query. */
static int WorldReady(void)
{
    return g_PcCoopGame && g_SavegamePtr != NULL && g_GameWork.gameState == GameState_InGame;
}

static int IsDeathState(int st)
{
    return st == PlayerState_Death || st == PlayerState_InstantDeath || st == PlayerState_Unk36 ||
           st == PlayerState_EnemyGrabPinnedFront || st == PlayerState_EnemyGrabPinnedBack ||
           st == PlayerState_OnFloorFront || st == PlayerState_OnFloorBehind;
}

static int RoomAt(int x, int z)
{
    return g_MapOverlayHdr.mapRoomIdxGet ? (int)g_MapOverlayHdr.mapRoomIdxGet(x, z) : 0;
}

static void SetVisible(int on)
{
    s_Model* m = &g_SysWork.playerWork.player.model;
    if (on) Model_AnimFlagsSet(m, AnimFlag_Visible);
    else    Model_AnimFlagsClear(m, AnimFlag_Visible);
}

/* Y from the floor under the player, if that chunk is loaded. */
static int ProbeGround(void)
{
    s_SubCharacter*    p = &g_SysWork.playerWork.player;
    s_CollisionSurface coll;
    Collision_SurfaceGet(&coll, p->position.vx, p->position.vz);
    if (coll.groundHeight == RS_NO_FLOOR)
    {
        return 0;
    }
    p->position.vy                    = coll.groundHeight;
    p->properties.player.groundHeight = coll.groundHeight;
    p->fallSpeed                      = 0;
    return 1;
}

static void Abort(void)
{
    SetVisible(1);
    if (s_phase == RS_SETTLE && s_graceLeft > 0)
    {
        g_Player_DisableDamage = false;
    }
    s_phase = RS_ALIVE;
}

/* ------------------------------------------------------------------ */
/* Engine hooks (player_control.c)                                     */
/* ------------------------------------------------------------------ */

int Pc_Respawn_HoldDeath(void)
{
    if (!InCoop() || HostGameOver() || s_hostGameOver)
    {
        if (s_phase == RS_HOLD || s_phase == RS_BLINK)
        {
            SH_DBG("[RESPAWN] nobody is left alive; the host's game over goes ahead");
            Abort();
        }
        return 0;
    }
    if (s_phase != RS_HOLD && s_phase != RS_BLINK)
    {
        s_phase   = RS_HOLD;
        s_elapsed = 0;
        s_waitWhy = 0;
        SH_DBG("[RESPAWN] died in co-op; back in %.0f s next to a partner who is out of combat",
               g_PcConfig.onlineRespawnSeconds + RS_BLINK_SECONDS);
    }
    return 1;
}

int Pc_Respawn_Holding(void)
{
    return s_phase == RS_HOLD || s_phase == RS_BLINK;
}

/* ------------------------------------------------------------------ */
/* Respawn                                                             */
/* ------------------------------------------------------------------ */

/* Nearest living co-op partner on this map who is out of combat, same room
 * preferred: a step behind them, facing their way. */
static int PickPartner(VECTOR3* pos, short* rot, char* how, int howCap)
{
    const int              myMap  = (int)g_SavegamePtr->mapIdx;
    const int              myRoom = (int)g_SavegamePtr->mapRoomIdx;
    const unsigned int     now    = ShNetPlat_Millis();
    const ShSessionMember* best   = NULL;
    int                    bestSame = 0;
    float                  bestD    = 0.0f;
    int                    i, n;

    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        int   same;
        float dx, dz, d;
        if (m == NULL || m->poseMs == 0 || m->steamId == ShSession_SelfId() || m->mapIdx != myMap)
        {
            continue;
        }
        if (!(m->flags & SHNET_PF_ALIVE) || (int)(now - m->poseMs) > RS_POSE_STALE_MS || !CombatClear(m, now))
        {
            continue;
        }
        same = (RoomAt(m->x, m->z) == myRoom);
        dx   = (float)(m->x - g_SysWork.playerWork.player.position.vx);
        dz   = (float)(m->z - g_SysWork.playerWork.player.position.vz);
        d    = dx * dx + dz * dz;
        if (best == NULL || (same && !bestSame) || (same == bestSame && d < bestD))
        {
            best = m; bestSame = same; bestD = d;
        }
    }
    if (best == NULL)
    {
        return 0;
    }

    pos->vx = best->x - Q12_MULT_FLOAT_PRECISE(Math_Sin(best->rotY), 0.8f);
    pos->vz = best->z - Q12_MULT_FLOAT_PRECISE(Math_Cos(best->rotY), 0.8f);
    pos->vy = best->y;
    {
        /* no floor a step behind them (wall, ledge): their exact spot. And the
         * spot must be real ground near the height they report: the position
         * is theirs to claim, and a void or a point inside geometry would put
         * us there on every respawn. Without that, the room entry is used. */
        s_CollisionSurface coll;
        Collision_SurfaceGet(&coll, pos->vx, pos->vz);
        if (coll.groundHeight == RS_NO_FLOOR)
        {
            pos->vx = best->x;
            pos->vz = best->z;
            Collision_SurfaceGet(&coll, pos->vx, pos->vz);
        }
        if (coll.groundHeight == RS_NO_FLOOR || ABS(coll.groundHeight - best->y) > Q12(1.5f))
        {
            return 0;
        }
        pos->vy = coll.groundHeight;
    }
    *rot = best->rotY;
    snprintf(how, (size_t)howCap, "next to %s%s", best->name, bestSame ? "" : " (another room)");
    return 1;
}

/* A living partner out of combat on another map: we go to them by a reboot
 * into that map (the inventory and flags come along). */
static const ShSessionMember* PartnerElsewhere(void)
{
    const unsigned int now = ShNetPlat_Millis();
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (PartnerAlive(m, now) && m->mapIdx >= 0 && m->mapIdx != (int)g_SavegamePtr->mapIdx &&
            (int)(now - m->poseMs) <= RS_POSE_STALE_MS && CombatClear(m, now))
        {
            return m;
        }
    }
    return NULL;
}

/* Someone we may come back next to right now? 1 here, 2 on another map
 * (the reboot was requested), 0 not yet (s_waitWhy says why). */
static int FindSpot(VECTOR3* pos, short* rot, char* how, int howCap)
{
    const ShSessionMember* far;
    if (PickPartner(pos, rot, how, howCap))
    {
        s_waitWhy = 0;
        return 1;
    }
    far = PartnerElsewhere();
    if (far != NULL)
    {
        extern void Pc_CoopMenu_RequestWarp(int mapIdx, int x, int z, short rotY);
        SH_DBG("[RESPAWN] no safe partner here; going to %s on map %d", far->name, far->mapIdx);
        Pc_CoopMenu_RequestWarp(far->mapIdx, far->x, far->z, far->rotY);
        SetVisible(1);
        s_phase   = RS_ALIVE; /* the reboot takes it from here */
        s_waitWhy = 0;
        return 2;
    }
    s_waitWhy = AnyoneElseAlive() ? 1 : 2;
    return 0;
}

static void Respawn(void)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;
    VECTOR3         pos;
    short           rot;
    char            how[96];
    int             spot = FindSpot(&pos, &rot, how, (int)sizeof(how));

    if (spot == 2)
    {
        return;
    }
    if (spot == 0)
    {
        /* The partner we were blinking out for stepped into a fight: wait again. */
        SetVisible(1);
        s_phase   = RS_HOLD;
        s_elapsed = SecondsQ12(g_PcConfig.onlineRespawnSeconds);
        return;
    }

    /* Place: what Chara_PositionSet does for a door arrival, minus the map
     * point bookkeeping (loading screen, paper map) that does not change. */
    p->position    = pos;
    p->rotation.vx = 0;
    p->rotation.vy = rot;
    p->rotation.vz = 0;
    p->fallSpeed   = 0;
    p->moveSpeed   = 0;
    g_SysWork.cameraAngleY = rot;
    {
        extern int g_DebugThirdPersonCam;
        extern s32 g_TpsCamYaw, g_TpsCamPitch;
        if (g_DebugThirdPersonCam)
        {
            g_TpsCamYaw   = rot;
            g_TpsCamPitch = 0;
        }
    }
    func_8007E9C4(); /* state machine reset: IsDead off, damage cleared, prev position = here */
    Game_MapRoomIdxUpdate();

    /* Stand up. */
    p->health = Q12(100.0f);
    g_MapOverlayHdr.playerControlUnfreeze(true);
    Player_AnimFlagsSet(AnimFlag_Unlocked | AnimFlag_Visible);

    vcSetCameraUseWarp(&p->position, g_SysWork.cameraAngleY);

    s_settled      = ProbeGround();
    s_settleFrames = RS_SETTLE_FRAMES;
    g_Player_DisableDamage = true;
    s_graceLeft            = RS_GRACE;
    s_phase   = RS_SETTLE;
    s_elapsed = 0;
    SH_DBG("[RESPAWN] back on your feet %s", how);
}

/* Put a living player somewhere else on this map (a shared cutscene's
 * start): the respawn's placement without the stand-up, then the same
 * settle (floor probe retries while the chunk streams in, a short grace). */
void ShNetRespawn_PlaceAt(const void* posv, short rot)
{
    s_SubCharacter* p   = &g_SysWork.playerWork.player;
    const VECTOR3*  pos = (const VECTOR3*)posv;

    if (pos == NULL)
    {
        return;
    }
    p->position    = *pos;
    p->rotation.vx = 0;
    p->rotation.vy = rot;
    p->rotation.vz = 0;
    p->fallSpeed   = 0;
    p->moveSpeed   = 0;
    g_SysWork.cameraAngleY = rot;
    {
        extern int g_DebugThirdPersonCam;
        extern s32 g_TpsCamYaw, g_TpsCamPitch;
        if (g_DebugThirdPersonCam)
        {
            g_TpsCamYaw   = rot;
            g_TpsCamPitch = 0;
        }
    }
    func_8007E9C4();
    Game_MapRoomIdxUpdate();
    if (s_phase == RS_HOLD || s_phase == RS_BLINK)
    {
        /* A story scene starts and we are dead: everyone is in it, so this
         * is the respawn, at its start. */
        p->health = Q12(100.0f);
        g_MapOverlayHdr.playerControlUnfreeze(true);
        SH_DBG("[RESPAWN] back on your feet for the scene");
    }
    Player_AnimFlagsSet(AnimFlag_Unlocked | AnimFlag_Visible);
    vcSetCameraUseWarp(&p->position, g_SysWork.cameraAngleY);

    s_settled      = ProbeGround();
    s_settleFrames = RS_SETTLE_FRAMES;
    g_Player_DisableDamage = true;
    s_graceLeft            = RS_GRACE;
    s_phase   = RS_SETTLE;
    s_elapsed = 0;
    SH_DBG("[RESPAWN] moved to (%d.%03d, %d.%03d) for a shared scene",
           pos->vx >> 12, ((pos->vx & 0xFFF) * 1000) >> 12, pos->vz >> 12, ((pos->vz & 0xFFF) * 1000) >> 12);
}

/* ------------------------------------------------------------------ */
/* Per frame                                                           */
/* ------------------------------------------------------------------ */

void ShNetRespawn_Tick(void)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;

    if (!WorldReady())
    {
        if (s_phase != RS_ALIVE)
        {
            Abort();
        }
        return;
    }

    {
        const unsigned int now = ShNetPlat_Millis();
        int i, n = ShSession_MemberCount();
        for (i = 0; i < n; i++)
        {
            const ShSessionMember* m = ShSession_Member(i);
            if (m != NULL && m->poseMs != 0 && m->steamId != ShSession_SelfId()) NoteCombat(m, now);
        }
    }

    /* Remember where this room was entered (first frame standing on a
     * measured floor after the map/room index changed). */
    if (s_phase == RS_ALIVE && g_SysWork.sysState == SysState_Gameplay)
    {
        const int map = (int)g_SavegamePtr->mapIdx, room = (int)g_SavegamePtr->mapRoomIdx;
        if (map != s_lastMap || room != s_lastRoom)
        {
            s_lastMap = map; s_lastRoom = room;
            s_entryValid = 0;
        }
        if (!s_entryValid && p->health > Q12(0.0f))
        {
            s_CollisionSurface coll;
            Collision_SurfaceGet(&coll, p->position.vx, p->position.vz);
            if (coll.groundHeight != RS_NO_FLOOR)
            {
                s_entryPos   = p->position;
                s_entryRot   = p->rotation.vy;
                s_entryMap   = map;
                s_entryRoom  = room;
                s_entryValid = 1;
            }
        }
    }

    switch (s_phase)
    {
    case RS_HOLD:
    case RS_BLINK:
        /* The engine took this death elsewhere: the session dropped so the
         * vanilla game over went ahead, a script reset the player, or we left. */
        if (g_SysWork.sysState == SysState_GameOver || !InCoop() ||
            !IsDeathState((int)g_SysWork.playerWork.extra.state))
        {
            Abort();
            return;
        }
        if (g_SysWork.sysState != SysState_Gameplay) /* menus: the clock waits */
        {
            return;
        }
        if (HostGameOver())
        {
            SH_DBG("[RESPAWN] nobody is left alive; the host's game over goes ahead");
            Abort();
            return;
        }
        s_elapsed += g_DeltaTime;
        if (s_phase == RS_HOLD)
        {
            if (s_elapsed >= SecondsQ12(g_PcConfig.onlineRespawnSeconds))
            {
                VECTOR3 pos;
                short   rot;
                char    how[96];
                s_elapsed = SecondsQ12(g_PcConfig.onlineRespawnSeconds); /* the clock is done; now we wait */
                if (FindSpot(&pos, &rot, how, (int)sizeof(how)) == 1)
                {
                    s_phase   = RS_BLINK;
                    s_elapsed = 0;
                }
            }
            return;
        }
        SetVisible(((s_elapsed / RS_BLINK_PERIOD) & 1) == 0);
        if (s_elapsed >= SecondsQ12(RS_BLINK_SECONDS))
        {
            Respawn();
        }
        return;

    case RS_SETTLE:
        if (!s_settled && s_settleFrames > 0)
        {
            s_settleFrames--;
            s_settled = ProbeGround();
        }
        if (s_graceLeft > 0 && g_SysWork.sysState == SysState_Gameplay)
        {
            s_graceLeft -= g_DeltaTime;
            if (s_graceLeft <= 0)
            {
                g_Player_DisableDamage = false;
            }
        }
        if (s_graceLeft <= 0 && (s_settled || s_settleFrames == 0))
        {
            s_phase = RS_ALIVE;
        }
        return;

    default:
        return;
    }
}

int ShNetRespawn_Active(void)
{
    return s_phase == RS_HOLD || s_phase == RS_BLINK;
}

const char* ShNetRespawn_Waiting(void)
{
    if (s_phase != RS_HOLD || s_elapsed < SecondsQ12(g_PcConfig.onlineRespawnSeconds))
    {
        return NULL;
    }
    if (s_waitWhy == 2)
    {
        return ShSession_IsHost() ? "Waiting for a partner" : "Waiting for the host";
    }
    return "Waiting to respawn (players in combat)";
}

float ShNetRespawn_Remaining(void)
{
    float rem = 0.0f;
    if (s_phase == RS_HOLD)
    {
        rem = g_PcConfig.onlineRespawnSeconds - (float)s_elapsed / 4096.0f + RS_BLINK_SECONDS;
    }
    else if (s_phase == RS_BLINK)
    {
        rem = RS_BLINK_SECONDS - (float)s_elapsed / 4096.0f;
    }
    return rem < 0.0f ? 0.0f : rem;
}

int ShNetRespawn_HideModel(void)
{
    return s_phase == RS_BLINK &&
           !(g_SysWork.playerWork.player.model.anim.flags & AnimFlag_Visible);
}

int ShNetRespawn_BlocksGameOver(void)
{
    return InCoop();
}

void ShNetRespawn_HostGameOver(void)
{
    if (ShSession_IsHost() || !InCoop() || !(s_phase == RS_HOLD || s_phase == RS_BLINK))
    {
        return;
    }
    SH_DBG("[RESPAWN] the host's game over: ours plays too");
    s_hostGameOver = 1;
}

/* The end of the game over (SysState_GameOver_Update's last step). In co-op
 * the session stays: the host is asked how to go on (Pc_CoopMenu_OpenGameOver)
 * and a guest waits on black until the host's choice reboots everyone
 * (SHNET_CTRL_RESTART) or the session ends, when the vanilla title follows. */
int Pc_CoopGameOver_Hold(void)
{
    s_hostGameOver = 0;
    if (!InCoop())
    {
        return 0;
    }
    if (ShSession_IsHost())
    {
        if (!Pc_CoopMenu_GameOverDecided() && !Pc_CoopMenu_IsOpen())
        {
            Pc_CoopMenu_OpenGameOver(g_GameWork.autosave.playerHealth > Q12(0.0f));
        }
        return 1;
    }
    Gfx_StringSetPosition(SCREEN_POSITION_X(25.0f), SCREEN_POSITION_Y(43.5f));
    Gfx_StringDraw("Waiting_for_the_host...", DEFAULT_MAP_MESSAGE_LENGTH);
    return 1;
}
