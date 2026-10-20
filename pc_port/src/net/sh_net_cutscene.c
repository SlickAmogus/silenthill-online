/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_cutscene.c - one cutscene for everyone (co-op).
 *
 * A cutscene is a row of the map's event table (s_EventData) whose handler
 * runs under SysState_EventCallback; the player who walks into its trigger
 * is the only one who sees it. Here, when a player's game dispatches such a
 * row and a scene actually starts (the cutscene flag or borders come up
 * within a second), that player tells the room which row, and where they
 * stand. Everyone else on the map is asked to join: the player is held
 * still and unhurtable while the question is up, and on yes (or after a
 * while, so an idle partner stays in step) is placed where the scene began
 * and the same row is started directly, bypassing the trigger's flag and
 * position gates. The scripts position the player and camera themselves.
 *
 * The identifier on the wire is (map, row): eventParam alone repeats across
 * rows, and a disabledEventFlag arriving early through S_WORLD_FL would make
 * the ordinary trigger path skip the row forever, which is why the start is
 * forced rather than simulated.
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h" /* g_MapEventSysState, g_MapEventParam */
#include "bodyprog/screen/cutscene_border.h"

#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_net_respawn.h"
#include "sh_net_cutscene.h"
#include "sh_log.h"

#define CUT_CONFIRM_MS 1000u   /* a dispatched row has this long to become a scene */
#define CUT_PROMPT_MS  12000u  /* then the question answers itself with yes */
#define CUT_DEDUPE_MS  10000u

/* Sender side: a row we dispatched, waiting to see a scene start. */
static int          s_pendRow = -1;
static unsigned int s_pendMs;

/* Receiver side. */
static int          s_promptRow = -1;
static int          s_promptMap = -1;
static VECTOR3      s_promptPos;
static short        s_promptRot;
static char         s_promptWho[SHSESSION_NAME_MAX];
static unsigned int s_promptMs;
static int          s_promptFroze;
static int          s_lastRow = -1, s_lastMap = -1;
static unsigned int s_lastMs;

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

static void Cut_Unfreeze(void)
{
    if (s_promptFroze)
    {
        g_MapOverlayHdr.playerControlUnfreeze(false);
        g_Player_DisableDamage = false;
        s_promptFroze          = 0;
    }
}

static void Cut_Join(void)
{
    const s_EventData* row;
    int                n = Cut_RowCount();

    Cut_Unfreeze();
    if (s_promptRow < 0 || s_promptRow >= n || (int)g_SavegamePtr->mapIdx != s_promptMap ||
        g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
    {
        SH_DBG("[CUTSCENE] could not join row %d (map %d): not in plain gameplay on that map", s_promptRow, s_promptMap);
        s_promptRow = -1;
        return;
    }
    row = &g_MapOverlayHdr.mapEvents[s_promptRow];
    if (row->sysState != SysState_EventCallback)
    {
        s_promptRow = -1;
        return;
    }
    /* Where the scene began for its starter; the script moves us from
     * there. Placement is the respawn's (camera warp, room index, floor). */
    ShNetRespawn_PlaceAt((const void*)&s_promptPos, s_promptRot);

    g_MapEventData     = (s_EventData*)row;
    g_MapEventSysState = (s32)row->sysState;
    g_MapEventParam    = row->eventParam;
    SysWork_StateSetNext(SysState_EventCallback);
    SH_DBG("[CUTSCENE] joined %s's scene: map %d row %d (param %d)", s_promptWho, s_promptMap, s_promptRow, (int)row->eventParam);
    s_promptRow = -1;
}

void ShNetCut_Tick(void)
{
    const unsigned int now = ShNetPlat_Millis();

    if (!Cut_Coop())
    {
        s_pendRow = -1;
        if (s_promptRow >= 0) { Cut_Unfreeze(); s_promptRow = -1; }
        return;
    }

    /* Sender: the dispatched row became a scene (or did not). */
    if (s_pendRow >= 0)
    {
        if (Cut_SceneLive() && g_GameWork.gameState == GameState_InGame)
        {
            const s_SubCharacter* p = &g_SysWork.playerWork.player;
            unsigned char         b[20];
            int                   o = 0, r;
            b[o++] = SHNET_CTRL_CUTSCENE;
            b[o++] = (unsigned char)g_SavegamePtr->mapIdx;
            b[o++] = (unsigned char)s_pendRow;
            ShnPutS32(b, &o, p->position.vx);
            ShnPutS32(b, &o, p->position.vy);
            ShnPutS32(b, &o, p->position.vz);
            ShnPutS16(b, &o, p->rotation.vy);
            for (r = 0; r < 2; r++)
            {
                ShSession_SendGame(SHNET_MSG_S_CTRL, b, o, 0, 1);
            }
            SH_DBG("[CUTSCENE] started map %d row %d; told the room", (int)g_SavegamePtr->mapIdx, s_pendRow);
            s_lastRow = s_pendRow; s_lastMap = (int)g_SavegamePtr->mapIdx; s_lastMs = now;
            s_pendRow = -1;
        }
        else if (now - s_pendMs > CUT_CONFIRM_MS)
        {
            s_pendRow = -1; /* an item take, a message: not a scene */
        }
    }

    /* Receiver: the question. */
    if (s_promptRow >= 0)
    {
        const s_ControllerConfig* cc = &g_GameWorkPtr->config.controllerConfig;
        if (g_GameWork.gameState != GameState_InGame || (int)g_SavegamePtr->mapIdx != s_promptMap ||
            Cut_SceneLive() || g_SysWork.playerWork.player.health <= 0)
        {
            /* Left the map, already in a scene of our own, or dead. */
            Cut_Unfreeze();
            s_promptRow = -1;
            return;
        }
        if (g_SysWork.sysState != SysState_Gameplay)
        {
            return; /* a menu or a text box; ask when it closes */
        }
        if (!s_promptFroze)
        {
            g_MapOverlayHdr.playerControlFreeze();
            g_Player_DisableDamage = true;
            s_promptFroze          = 1;
        }
        if (g_Controller0->clickedBtnFlags & cc->cancel)
        {
            SH_DBG("[CUTSCENE] declined %s's scene", s_promptWho);
            Cut_Unfreeze();
            s_promptRow = -1;
            return;
        }
        if ((g_Controller0->clickedBtnFlags & (cc->enter | cc->action)) || now - s_promptMs > CUT_PROMPT_MS)
        {
            Cut_Join();
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
    if (map != (int)g_SavegamePtr->mapIdx || row >= Cut_RowCount() ||
        g_MapOverlayHdr.mapEvents[row].sysState != SysState_EventCallback)
    {
        return;
    }
    if (pos.vx > SHNET_COORD_MAX || pos.vx < -SHNET_COORD_MAX || pos.vy > SHNET_COORD_MAX || pos.vy < -SHNET_COORD_MAX ||
        pos.vz > SHNET_COORD_MAX || pos.vz < -SHNET_COORD_MAX)
    {
        return;
    }
    /* Sent twice, and the same scene can be announced by two players. */
    if (row == s_lastRow && map == s_lastMap && now - s_lastMs < CUT_DEDUPE_MS)
    {
        return;
    }
    s_lastRow = row; s_lastMap = map; s_lastMs = now;
    if (Cut_SceneLive() || s_promptRow >= 0)
    {
        return; /* already in one, or already asked */
    }
    s_promptRow = row;
    s_promptMap = map;
    s_promptPos = pos;
    s_promptRot = rot;
    s_promptMs  = now;
    snprintf(s_promptWho, sizeof(s_promptWho), "%s", Cut_NameOf(from));
    SH_DBG("[CUTSCENE] %s started a scene (map %d row %d); asking", s_promptWho, map, row);
}

int ShNetCut_PromptText(char* out, int cap, int* secondsLeft)
{
    if (s_promptRow < 0 || !s_promptFroze || !out || cap <= 0)
    {
        return 0;
    }
    snprintf(out, (size_t)cap, "%s started a scene. Join it?", s_promptWho);
    if (secondsLeft)
    {
        unsigned int el = ShNetPlat_Millis() - s_promptMs;
        *secondsLeft = (el >= CUT_PROMPT_MS) ? 0 : (int)((CUT_PROMPT_MS - el + 999u) / 1000u);
    }
    return 1;
}
