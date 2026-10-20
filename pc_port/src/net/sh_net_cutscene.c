/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_cutscene.c - one cutscene for everyone (co-op).
 *
 * A cutscene is a row of the map's event table (s_EventData) whose handler
 * runs under SysState_EventCallback; the player who walks into its trigger
 * is the only one who sees it. Here, when a player's game dispatches such a
 * row and a scene actually starts (the cutscene flag or borders come up
 * within a second), that player tells the room which row, and where they
 * stand, and keeps telling it every couple of seconds while the scene runs
 * so a partner who is still loading the map hears it too. Everyone else on
 * the map is pulled in at once: placed where the scene began and started on
 * the same row directly, bypassing the trigger's flag and position gates.
 * The scripts position the player and camera themselves.
 *
 * Map-entry scenes (TriggerType_None rows, which fire the moment a map is
 * up) are held back while a partner is still loading, so the opening of the
 * game starts for everyone within a frame or two instead of the host
 * watching it alone while the guest's window is still booting.
 *
 * The identifier on the wire is (map, row): eventParam alone repeats across
 * rows, and a disabledEventFlag arriving early through S_WORLD_FL would make
 * the ordinary trigger path skip the row forever, which is why the start is
 * forced rather than simulated.
 *
 * The alley (map0_s00) is the one place where dying is the story: Harry's
 * death there is what takes him to the cafe. A partner's death on that map
 * is therefore everyone's death, so the cafe scene starts for the whole
 * room at once. Elsewhere a death is a respawn (sh_net_respawn.c).
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/math/math.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_MapEventSysState, g_MapEventParam */
#include "bodyprog/screen/cutscene_border.h"

#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_net_respawn.h"
#include "sh_net_cutscene.h"
#include "sh_log.h"

#define CUT_CONFIRM_MS   1000u  /* a dispatched row has this long to become a scene */
#define CUT_JOIN_WAIT_MS 12000u /* a join waits this long for a menu or text box to close */
#define CUT_DEDUPE_MS    60000u /* the same row announced again within this is the same scene */
#define CUT_REANNOUNCE_MS 2000u
#define CUT_HOLD_MS      20000u /* a map-entry scene waits this long for loading partners */
#define CUT_DIE_WAIT_MS  10000u

/* Sender side: a row we dispatched, waiting to see a scene start. */
static int          s_pendRow = -1;
static unsigned int s_pendMs;

/* Sender side: the scene we are in, repeated to late arrivals. */
static int          s_liveRow = -1, s_liveMap = -1;
static VECTOR3      s_livePos;
static short        s_liveRot;
static unsigned int s_liveSentMs;

/* Receiver side: a scene to join. */
static int          s_joinRow = -1;
static int          s_joinMap = -1;
static VECTOR3      s_joinPos;
static short        s_joinRot;
static char         s_joinWho[SHSESSION_NAME_MAX];
static unsigned int s_joinMs;
static int          s_lastRow = -1, s_lastMap = -1;
static unsigned int s_lastMs;

/* Map-entry hold. */
static int          s_holdFroze;
static int          s_heldThisFrame;
static unsigned int s_holdSince;
static int          s_holdRow = -1;
static char         s_holdWho[SHSESSION_NAME_MAX];

/* The alley death. */
static int          s_dieReq;
static unsigned int s_dieMs;
static unsigned int s_dieSentMs;
static char         s_dieWho[SHSESSION_NAME_MAX];

static int Cut_Coop(void)
{
    extern int g_PcCoopGame;
    return g_PcCoopGame && ShSession_Active() && ShSession_MemberCount() >= 2;
}

static int Cut_RowCount(void)
{
    int n = 0;
    if (g_MapOverlayHdr.mapEvents == NULL) return 0;
    while (n < 128 && g_MapOverlayHdr.mapEvents[n].triggerType != NO_VALUE) n++;
    return n;
}

static int Cut_SceneLive(void)
{
    return (g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
           g_SysWork.cutsceneBorderState != CutsceneBorderState_None;
}

static const char* Cut_NameOf(unsigned long long id)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == id) return m->name;
    }
    return "A partner";
}

static void Cut_Announce(int map, int row, const VECTOR3* pos, short rot)
{
    unsigned char b[20];
    int           o = 0;
    b[o++] = SHNET_CTRL_CUTSCENE;
    b[o++] = (unsigned char)map;
    b[o++] = (unsigned char)row;
    ShnPutS32(b, &o, pos->vx);
    ShnPutS32(b, &o, pos->vy);
    ShnPutS32(b, &o, pos->vz);
    ShnPutS16(b, &o, rot);
    ShSession_SendGame(SHNET_MSG_S_CTRL, b, o, 0, 1);
}

void ShNetCut_OnDispatch(void)
{
    if (!Cut_Coop() || g_MapEventSysState != SysState_EventCallback ||
        g_MapEventData == NULL || g_MapOverlayHdr.mapEvents == NULL)
    {
        return;
    }
    s_pendRow = (int)(g_MapEventData - g_MapOverlayHdr.mapEvents);
    s_pendMs  = ShNetPlat_Millis();
    if (s_pendRow < 0 || s_pendRow >= 128)
    {
        s_pendRow = -1;
    }
}

/* A partner who is between worlds: loading a map, or in a full-screen menu
 * (their pose then carries no map). The first such name, for the screen. */
static int Cut_SomeoneLoading(char* who, int cap)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || m->steamId == ShSession_SelfId() || !m->linked)
        {
            continue;
        }
        if (m->mapIdx < 0)
        {
            if (who) snprintf(who, (size_t)cap, "%s", m->name[0] ? m->name : "a partner");
            return 1;
        }
    }
    return 0;
}

int ShNetCut_HoldDispatch(void)
{
    const unsigned int now = ShNetPlat_Millis();
    int                row;

    if (!Cut_Coop() || g_MapEventSysState != SysState_EventCallback ||
        g_MapEventData == NULL || g_MapOverlayHdr.mapEvents == NULL ||
        g_MapEventData->triggerType != TriggerType_None ||
        g_GameWork.gameState != GameState_InGame)
    {
        return 0;
    }
    row = (int)(g_MapEventData - g_MapOverlayHdr.mapEvents);
    if (row != s_holdRow)
    {
        s_holdRow   = row;
        s_holdSince = now;
    }
    if (now - s_holdSince > CUT_HOLD_MS || !Cut_SomeoneLoading(s_holdWho, (int)sizeof(s_holdWho)))
    {
        return 0;
    }
    if (!s_holdFroze)
    {
        g_MapOverlayHdr.playerControlFreeze();
        g_Player_DisableDamage = true;
        s_holdFroze            = 1;
        SH_DBG("[CUTSCENE] holding map %d row %d for %s", (int)g_SavegamePtr->mapIdx, row, s_holdWho);
    }
    s_heldThisFrame    = 1;
    g_MapEventSysState = SysState_Invalid; /* Event_Update offers the row again next frame */
    return 1;
}

static void Cut_Unhold(void)
{
    if (s_holdFroze)
    {
        g_MapOverlayHdr.playerControlUnfreeze(false);
        g_Player_DisableDamage = false;
        s_holdFroze            = 0;
        SH_DBG("[CUTSCENE] hold released");
    }
    s_holdRow = -1;
}

static void Cut_Join(void)
{
    const s_EventData* row;
    int                n = Cut_RowCount();

    if (s_joinRow < 0 || s_joinRow >= n || (int)g_SavegamePtr->mapIdx != s_joinMap ||
        g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
    {
        SH_DBG("[CUTSCENE] could not join row %d (map %d): not in plain gameplay on that map", s_joinRow, s_joinMap);
        s_joinRow = -1;
        return;
    }
    row = &g_MapOverlayHdr.mapEvents[s_joinRow];
    if (row->sysState != SysState_EventCallback)
    {
        s_joinRow = -1;
        return;
    }
    Cut_Unhold();
    /* Where the scene began for its starter; the script moves us from
     * there. Placement is the respawn's (camera warp, room index, floor). */
    ShNetRespawn_PlaceAt((const void*)&s_joinPos, s_joinRot);

    g_MapEventData     = (s_EventData*)row;
    g_MapEventSysState = (s32)row->sysState;
    g_MapEventParam    = row->eventParam;
    SysWork_StateSetNext(SysState_EventCallback);
    {
        char line[96];
        snprintf(line, sizeof(line), "Joined %s's scene", s_joinWho);
        ShNet_PostLocalEvent(line);
    }
    SH_DBG("[CUTSCENE] joined %s's scene: map %d row %d (param %d)", s_joinWho, s_joinMap, s_joinRow, (int)row->eventParam);
    s_joinRow = -1;
}

/* The alley death, applied to us: a monster's blow worth a full bar, through
 * the same door a partner's bullet comes in (Pc_NetHit_ApplyIncoming), so the
 * death plays out as one and ends where the alley's death ends. */
static void Cut_Die(void)
{
    s_SubCharacter* me = &g_SysWork.playerWork.player;

    g_Player_DisableDamage = false;
    me->damage.amount      = Q12(400.0f);
    me->damage.position.vx = 0;
    me->damage.position.vy = 0;
    me->damage.position.vz = 0;
    me->attackReceived     = 43;
    me->field_40           = NO_VALUE;
    {
        char line[96];
        snprintf(line, sizeof(line), "%s fell in the alley; so do you", s_dieWho);
        ShNet_PostLocalEvent(line);
    }
    SH_DBG("[CUTSCENE] %s died in the alley: dying too", s_dieWho);
    s_dieReq = 0;
}

void ShNetCut_Tick(void)
{
    const unsigned int now = ShNetPlat_Millis();

    if (!Cut_Coop())
    {
        s_pendRow = -1;
        s_liveRow = -1;
        s_joinRow = -1;
        s_dieReq  = 0;
        Cut_Unhold();
        s_heldThisFrame = 0;
        return;
    }

    /* The hold is asserted by the dispatch site each frame it applies; a
     * frame without it means the row went away (its flag arrived, the map
     * changed) or the wait timed out and the scene is starting on its own. */
    if (s_holdFroze && !s_heldThisFrame)
    {
        Cut_Unhold();
    }
    s_heldThisFrame = 0;

    /* Sender: the dispatched row became a scene (or did not). */
    if (s_pendRow >= 0)
    {
        if (Cut_SceneLive() && g_GameWork.gameState == GameState_InGame)
        {
            const s_SubCharacter* p = &g_SysWork.playerWork.player;
            int                   r;
            s_liveRow = s_pendRow;
            s_liveMap = (int)g_SavegamePtr->mapIdx;
            s_livePos = p->position;
            s_liveRot = p->rotation.vy;
            for (r = 0; r < 2; r++)
            {
                Cut_Announce(s_liveMap, s_liveRow, &s_livePos, s_liveRot);
            }
            s_liveSentMs = now;
            SH_DBG("[CUTSCENE] started map %d row %d; told the room", s_liveMap, s_liveRow);
            s_lastRow = s_pendRow; s_lastMap = s_liveMap; s_lastMs = now;
            s_pendRow = -1;
        }
        else if (now - s_pendMs > CUT_CONFIRM_MS)
        {
            s_pendRow = -1; /* an item take, a message: not a scene */
        }
    }

    /* Sender: while the scene runs, repeat it for whoever arrives late. */
    if (s_liveRow >= 0)
    {
        if (!Cut_SceneLive() || g_GameWork.gameState != GameState_InGame ||
            (int)g_SavegamePtr->mapIdx != s_liveMap)
        {
            s_liveRow = -1;
        }
        else if (now - s_liveSentMs >= CUT_REANNOUNCE_MS)
        {
            Cut_Announce(s_liveMap, s_liveRow, &s_livePos, s_liveRot);
            s_liveSentMs = now;
        }
    }

    /* Receiver: join as soon as plain gameplay allows. */
    if (s_joinRow >= 0)
    {
        if (g_GameWork.gameState != GameState_InGame || (int)g_SavegamePtr->mapIdx != s_joinMap ||
            Cut_SceneLive() || g_SysWork.playerWork.player.health <= 0 ||
            now - s_joinMs > CUT_JOIN_WAIT_MS)
        {
            /* Left the map, already in a scene of our own, dead, or a menu
             * stayed up too long. */
            s_joinRow = -1;
        }
        else if (g_SysWork.sysState == SysState_Gameplay)
        {
            Cut_Join();
        }
    }

    /* The alley death, once we are back in plain gameplay. */
    if (s_dieReq)
    {
        if (g_GameWork.gameState != GameState_InGame || (int)g_SavegamePtr->mapIdx != MapIdx_MAP0_S00 ||
            g_SysWork.playerWork.player.health <= 0 || now - s_dieMs > CUT_DIE_WAIT_MS)
        {
            s_dieReq = 0;
        }
        else if (g_SysWork.sysState == SysState_Gameplay && !Cut_SceneLive())
        {
            Cut_Die();
        }
    }
}

void ShNetCut_CtrlIn(unsigned long long from, const unsigned char* p, int len)
{
    const unsigned int now = ShNetPlat_Millis();
    int                o = 1, map, row;
    VECTOR3            pos;
    short              rot;

    if (len < 17 || !Cut_Coop() || from == ShSession_SelfId())
    {
        return;
    }
    map = p[o++];
    row = p[o++];
    pos.vx = ShnGetS32(p, &o);
    pos.vy = ShnGetS32(p, &o);
    pos.vz = ShnGetS32(p, &o);
    rot    = (short)ShnGetS16(p, &o);
    if (g_SavegamePtr == NULL || map != (int)g_SavegamePtr->mapIdx || row >= Cut_RowCount() ||
        g_MapOverlayHdr.mapEvents[row].sysState != SysState_EventCallback)
    {
        return;
    }
    if (pos.vx > SHNET_COORD_MAX || pos.vx < -SHNET_COORD_MAX || pos.vy > SHNET_COORD_MAX || pos.vy < -SHNET_COORD_MAX ||
        pos.vz > SHNET_COORD_MAX || pos.vz < -SHNET_COORD_MAX)
    {
        return;
    }
    /* Repeated every couple of seconds while it runs, and the same scene
     * can be announced by two players, or be our own. */
    if (row == s_lastRow && map == s_lastMap && now - s_lastMs < CUT_DEDUPE_MS)
    {
        return;
    }
    if (Cut_SceneLive() || s_joinRow >= 0)
    {
        return; /* already in one, or already joining */
    }
    s_lastRow = row; s_lastMap = map; s_lastMs = now;
    s_joinRow = row;
    s_joinMap = map;
    s_joinPos = pos;
    s_joinRot = rot;
    s_joinMs  = now;
    snprintf(s_joinWho, sizeof(s_joinWho), "%s", Cut_NameOf(from));
    SH_DBG("[CUTSCENE] %s started a scene (map %d row %d); joining", s_joinWho, map, row);
}

void ShNetCut_OnSharedDeath(void)
{
    unsigned char b[2];
    int           r;

    if (!Cut_Coop() || g_SavegamePtr == NULL || (int)g_SavegamePtr->mapIdx != MapIdx_MAP0_S00)
    {
        return;
    }
    b[0] = SHNET_CTRL_DIE;
    b[1] = (unsigned char)g_SavegamePtr->mapIdx;
    for (r = 0; r < 2; r++)
    {
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, 2, 0, 1);
    }
    s_dieSentMs = ShNetPlat_Millis();
    s_dieReq    = 0;
    SH_DBG("[CUTSCENE] died in the alley; told the room");
}

void ShNetCut_DieIn(unsigned long long from, const unsigned char* p, int len)
{
    const unsigned int now = ShNetPlat_Millis();

    if (len < 2 || !Cut_Coop() || from == ShSession_SelfId() || g_SavegamePtr == NULL)
    {
        return;
    }
    /* Only the alley, and only while we are there too. */
    if (p[1] != MapIdx_MAP0_S00 || (int)g_SavegamePtr->mapIdx != MapIdx_MAP0_S00)
    {
        return;
    }
    /* Our own death (echoed back by a partner dying of it) and repeats. */
    if ((s_dieSentMs && now - s_dieSentMs < CUT_DIE_WAIT_MS) || s_dieReq)
    {
        return;
    }
    if (g_GameWork.gameState == GameState_InGame && g_SysWork.playerWork.player.health <= 0)
    {
        return; /* already dying */
    }
    s_dieReq = 1;
    s_dieMs  = now;
    snprintf(s_dieWho, sizeof(s_dieWho), "%s", Cut_NameOf(from));
    SH_DBG("[CUTSCENE] %s died in the alley", s_dieWho);
}

int ShNetCut_InScene(void)
{
    return Cut_Coop() && g_GameWork.gameState == GameState_InGame && Cut_SceneLive();
}

int ShNetCut_SceneSlot(unsigned long long id)
{
    const unsigned long long me = ShSession_SelfId();
    int                      i, n = ShSession_MemberCount();
    int                      rankMe = 0, rankId = 0, found = 0;
    const int                myMap = (g_SavegamePtr) ? (int)g_SavegamePtr->mapIdx : -1;

    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m)
        {
            continue;
        }
        if (m->steamId == id)
        {
            found = 1;
        }
        /* Everyone standing in this scene: partners whose pose says so, and us. */
        if (m->steamId != me && (m->poseMs == 0 || m->mapIdx != myMap || !(m->flags & SHNET_PF_CUTSCENE)))
        {
            continue;
        }
        if (m->steamId < me) rankMe++;
        if (m->steamId < id) rankId++;
    }
    if (!found)
    {
        return 0;
    }
    return rankId - rankMe;
}

int ShNetCut_StatusText(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return 0;
    }
    if (s_holdFroze)
    {
        snprintf(out, (size_t)cap, "Waiting for %s to arrive...", s_holdWho);
        return 1;
    }
    if (s_joinRow >= 0)
    {
        snprintf(out, (size_t)cap, "Joining %s's scene...", s_joinWho);
        return 1;
    }
    return 0;
}
