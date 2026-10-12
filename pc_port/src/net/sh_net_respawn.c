/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_respawn.c - co-op respawn. Design in sh_net_respawn.h.
 *
 * Phases:  ALIVE --death anim ends--> HOLD --delay--> BLINK --1.5 s--> SETTLE --> ALIVE
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
    if (!InCoop())
    {
        return 0;
    }
    if (s_phase != RS_HOLD && s_phase != RS_BLINK)
    {
        s_phase   = RS_HOLD;
        s_elapsed = 0;
        SH_DBG("[RESPAWN] died in co-op; back in %.0f s",
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

/* Nearest living co-op partner on this map, same room preferred: a step
 * behind them, facing their way. */
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
        if (!(m->flags & SHNET_PF_ALIVE) || (int)(now - m->poseMs) > RS_POSE_STALE_MS)
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
        /* no floor a step behind them (wall, ledge): their exact spot */
        s_CollisionSurface coll;
        Collision_SurfaceGet(&coll, pos->vx, pos->vz);
        if (coll.groundHeight == RS_NO_FLOOR)
        {
            pos->vx = best->x;
            pos->vz = best->z;
        }
    }
    *rot = best->rotY;
    snprintf(how, (size_t)howCap, "next to %s%s", best->name, bestSame ? "" : " (another room)");
    return 1;
}

static void Respawn(void)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;
    VECTOR3         pos;
    short           rot;
    char            how[96];

    if (!PickPartner(&pos, &rot, how, (int)sizeof(how)))
    {
        if (s_entryValid && s_entryMap == (int)g_SavegamePtr->mapIdx && s_entryRoom == (int)g_SavegamePtr->mapRoomIdx)
        {
            pos = s_entryPos;
            rot = s_entryRot;
            snprintf(how, sizeof(how), "where you entered this room");
        }
        else
        {
            pos = p->position;
            rot = p->rotation.vy;
            snprintf(how, sizeof(how), "where you fell");
        }
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
        s_elapsed += g_DeltaTime;
        if (s_phase == RS_HOLD)
        {
            if (s_elapsed >= SecondsQ12(g_PcConfig.onlineRespawnSeconds))
            {
                s_phase   = RS_BLINK;
                s_elapsed = 0;
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
