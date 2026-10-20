/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_cutscene.c - one cutscene for everyone (co-op).
 *
 * A cutscene is a row of the map's event table (s_EventData) whose handler
 * runs under SysState_EventCallback; the player who walks into its trigger
 * is the only one who sees it. Here, when a player's game dispatches such a
 * row and a scene actually starts, that player tells the room which row and
 * where they stand, and keeps telling it every couple of seconds while the
 * scene runs. Everyone else on the map is pulled in at once: placed where
 * the scene began and started on the same row directly, bypassing the
 * trigger's flag and position gates. The scripts position the player and
 * camera themselves.
 *
 * Map-entry scenes (TriggerType_None rows, which fire the moment a map is
 * up) start together: the host holds its own back while a partner is still
 * loading the map or sitting in a movie or a menu, announces the row the
 * frame it dispatches, and a guest holds its own copy of the row until that
 * announcement comes (or the host turns out to be elsewhere). On one PC the
 * two halves then start within a frame of each other; over the network,
 * within the one-way latency.
 *
 * Story doors (s_gates below: a door or gate with a scene, or a point of no
 * return, behind it) wait instead of pulling: whoever uses one stands at it
 * under "Waiting for other players..." until every partner on the map who
 * has not been through it is close by. Using it again goes on without them.
 *
 * Seats: a scripted scene moves every player along the same path, so they
 * would stand inside each other. Each player's game offsets its OWN model
 * by its seat (the host sits where the script put it, guests a step aside;
 * a scene can have its own table), for the drawing and for the pose it
 * sends, so every screen shows the same arrangement. The simulated position
 * is never touched.
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

#define CUT_CONFIRM_MS    1000u  /* a dispatched row has this long to become a scene */
#define CUT_JOIN_WAIT_MS  12000u /* a join waits this long for a menu or text box to close */
#define CUT_DEDUPE_MS     60000u /* the same row announced again within this is the same scene */
#define CUT_REANNOUNCE_MS 2000u
#define CUT_HOLD_MS       20000u /* a map-entry scene waits this long for partners */
#define CUT_DIE_WAIT_MS   10000u
#define CUT_GATE_WAIT_MS  90000u /* a story door waits this long for partners */
#define CUT_GATE_GO_MS    5000u  /* a released door has this long to dispatch */
#define CUT_GATE_NEAR     Q12(7.0f)
#define CUT_POSE_FRESH_MS 2000u  /* a partner whose pose is older than this is not ready */

/* Story doors: rows (LoadRoom / LoadOverlay) that wait for the partners who
 * have not been through them. Only doors with a scene or a point of no return
 * behind them belong here; an ordinary door is walked through alone. */
typedef struct { unsigned char map, row; } CutGate;
static const CutGate s_gates[] = {
    { MapIdx_MAP0_S00, 15 }, /* the alley's first gate: alley2 and its own cameras are past it */
    { MapIdx_MAP0_S00, 17 }, /* the second gate: the dark, the lighter and the end of the alley */
};

/* Seats, in tenths of a metre from the scripted spot: ahead along the facing,
 * to the right across it, and a turn in degrees. Seat 0 is the host's and is
 * always the scripted spot itself. */
typedef struct { short fwd, side, turn; } CutSeat;
#define CUT_SEATS 8
static const CutSeat s_seatsDefault[CUT_SEATS] = {
    { 0, 0, 0 }, { 0, 7, 0 }, { 0, -7, 0 }, { 0, 14, 0 }, { 0, -14, 0 }, { 0, 21, 0 }, { 0, -21, 0 }, { 0, 28, 0 }
};
/* The cafe (map0_s01 row 3): Harry sits at the aisle end of the booth's
 * bench, so the next player sits deeper on the same bench and the rest take
 * the seats across the table, deeper than Cybil's, facing back. */
static const CutSeat s_seatsCafe[CUT_SEATS] = {
    { 0, 0, 0 }, { 0, -7, 0 }, { 14, -7, 180 }, { 14, -14, 180 }, { 0, -14, 0 }, { 14, 0, 180 }, { 0, -21, 0 }, { 14, -21, 180 }
};

/* Sender side: a row we dispatched, waiting to see a scene start. */
static int          s_pendRow = -1;
static unsigned int s_pendMs;

/* The scene we are in (started or joined), repeated to late arrivals. */
static int          s_liveRow = -1, s_liveMap = -1;
static VECTOR3      s_livePos;
static short        s_liveRot;
static unsigned int s_liveSentMs, s_liveSince;

/* Receiver side: a scene to join. */
static int          s_joinRow = -1;
static int          s_joinMap = -1;
static VECTOR3      s_joinPos;
static short        s_joinRot;
static char         s_joinWho[SHSESSION_NAME_MAX];
static unsigned int s_joinMs;
static int          s_lastRow = -1, s_lastMap = -1;
static unsigned int s_lastMs;

/* The player stands still for a map-entry hold or a story door. */
static int          s_frozen;

/* Map-entry hold. */
static int          s_holdFroze;
static int          s_heldThisFrame;
static unsigned int s_holdSince;
static int          s_holdRow = -1;
static char         s_holdWho[SHSESSION_NAME_MAX];

/* Story door. */
static const s_EventData* s_gateRow;
static int          s_gateMap, s_gateIdx;
static unsigned int s_gateSince;
static int          s_gateFroze, s_gateGo, s_gateInject;
typedef struct { unsigned long long id; unsigned char map, row; } CutPassed;
#define CUT_PASSED_MAX 32
static CutPassed    s_passed[CUT_PASSED_MAX];
static int          s_passedN;
static unsigned long long s_gateToldBy;
static int          s_gateToldMap = -1, s_gateToldRow = -1;
static unsigned int s_gateToldMs;

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

static int Cut_MyMap(void)
{
    return (g_SavegamePtr) ? (int)g_SavegamePtr->mapIdx : -1;
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

static const ShSessionMember* Cut_MemberById(unsigned long long id)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == id) return m;
    }
    return NULL;
}

static const char* Cut_NameOf(unsigned long long id)
{
    const ShSessionMember* m = Cut_MemberById(id);
    return (m && m->name[0]) ? m->name : "A partner";
}

static void Cut_Freeze(void)
{
    if (!s_frozen)
    {
        g_MapOverlayHdr.playerControlFreeze();
        g_Player_DisableDamage = true;
        s_frozen               = 1;
    }
}

/* Thaws once neither the map-entry hold nor a door needs the player still. */
static void Cut_Thaw(void)
{
    if (s_frozen && !s_holdFroze && !s_gateFroze)
    {
        g_MapOverlayHdr.playerControlUnfreeze(false);
        g_Player_DisableDamage = false;
        s_frozen               = 0;
    }
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

/* The scene is ours from here: tell the room, twice. */
static void Cut_GoLive(int row, unsigned int now)
{
    const s_SubCharacter* p = &g_SysWork.playerWork.player;
    int                   r;

    s_liveRow = row;
    s_liveMap = Cut_MyMap();
    s_livePos = p->position;
    s_liveRot = p->rotation.vy;
    for (r = 0; r < 2; r++)
    {
        Cut_Announce(s_liveMap, s_liveRow, &s_livePos, s_liveRot);
    }
    s_liveSentMs = now;
    s_liveSince  = now;
    s_lastRow = row; s_lastMap = s_liveMap; s_lastMs = now;
    SH_DBG("[CUTSCENE] started map %d row %d; told the room", s_liveMap, s_liveRow);
}

/* ------------------------------------------------------------------ */
/* Story doors                                                         */
/* ------------------------------------------------------------------ */

static int Cut_IsGate(int map, int row)
{
    unsigned i;
    for (i = 0; i < sizeof(s_gates) / sizeof(s_gates[0]); i++)
    {
        if (s_gates[i].map == map && s_gates[i].row == row) return 1;
    }
    return 0;
}

static int Cut_Passed(unsigned long long id, int map, int row)
{
    int i;
    for (i = 0; i < s_passedN; i++)
    {
        if (s_passed[i].id == id && s_passed[i].map == map && s_passed[i].row == row) return 1;
    }
    return 0;
}

static void Cut_PassedAdd(unsigned long long id, int map, int row)
{
    if (Cut_Passed(id, map, row)) return;
    if (s_passedN == CUT_PASSED_MAX)
    {
        memmove(&s_passed[0], &s_passed[1], sizeof(s_passed[0]) * (CUT_PASSED_MAX - 1));
        s_passedN--;
    }
    s_passed[s_passedN].id  = id;
    s_passed[s_passedN].map = (unsigned char)map;
    s_passed[s_passedN].row = (unsigned char)row;
    s_passedN++;
}

/* state 1: waiting at it; 2: went through. */
static void Cut_GateTell(int map, int row, int state)
{
    unsigned char b[4];
    int           r;
    b[0] = SHNET_CTRL_GATE;
    b[1] = (unsigned char)map;
    b[2] = (unsigned char)row;
    b[3] = (unsigned char)state;
    for (r = 0; r < (state == 2 ? 2 : 1); r++)
    {
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, 4, 0, 1);
    }
}

/* A partner on this map who has not been through the door and is not beside
 * us (or is still loading the map). The first such name, for the log. */
static int Cut_GateWaitingOn(int map, int row, char* who, int cap)
{
    const s_SubCharacter* me = &g_SysWork.playerWork.player;
    int                   i, n = ShSession_MemberCount();

    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        int                    far;
        if (!m || m->steamId == ShSession_SelfId() || !m->linked) continue;
        if (Cut_Passed(m->steamId, map, row)) continue;
        if (m->mapIdx >= 0 && m->mapIdx != map) continue; /* elsewhere: cannot come */
        if (m->healthPct1 == 1) continue;                  /* dead */
        far = (m->mapIdx < 0 || m->poseMs == 0 ||
               ABS(m->x - me->position.vx) > CUT_GATE_NEAR || ABS(m->z - me->position.vz) > CUT_GATE_NEAR);
        if (far)
        {
            if (who) snprintf(who, (size_t)cap, "%s", m->name[0] ? m->name : "a partner");
            return 1;
        }
    }
    return 0;
}

static void Cut_GateEnd(void)
{
    s_gateRow   = NULL;
    s_gateGo    = 0;
    s_gateFroze = 0;
    Cut_Thaw();
}

/* The door's row is about to dispatch. */
static int Cut_GateHold(int map, int row, unsigned int now)
{
    char who[SHSESSION_NAME_MAX];

    if (s_gateRow == g_MapEventData)
    {
        /* Used again while waiting: go on without them. */
        SH_DBG("[CUTSCENE] map %d row %d: going through without the others", map, row);
        Cut_GateEnd();
        Cut_GateTell(map, row, 2);
        return 0;
    }
    if (!Cut_GateWaitingOn(map, row, who, (int)sizeof(who)))
    {
        Cut_GateTell(map, row, 2);
        return 0;
    }
    s_gateRow   = g_MapEventData;
    s_gateMap   = map;
    s_gateIdx   = row;
    s_gateSince = now;
    s_gateFroze = 1;
    Cut_Freeze();
    Cut_GateTell(map, row, 1);
    SH_DBG("[CUTSCENE] waiting at map %d row %d (a story door) for %s", map, row, who);
    g_MapEventSysState = SysState_Invalid;
    return 1;
}

void ShNetCut_GateInject(void)
{
    if (!s_gateGo || s_gateRow == NULL) return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay ||
        g_MapEventSysState != SysState_Invalid || Cut_MyMap() != s_gateMap)
    {
        return;
    }
    g_MapEventData     = (s_EventData*)s_gateRow;
    g_MapEventSysState = (s32)s_gateRow->sysState;
    g_MapEventParam    = s_gateRow->eventParam;
    Cut_GateTell(s_gateMap, s_gateIdx, 2);
    SH_DBG("[CUTSCENE] going through map %d row %d", s_gateMap, s_gateIdx);
    s_gateInject = 1;
    Cut_GateEnd();
}

void ShNetCut_GateIn(unsigned long long from, const unsigned char* p, int len)
{
    const unsigned int now = ShNetPlat_Millis();
    int                map, row, state;

    if (len < 4 || !Cut_Coop() || from == ShSession_SelfId()) return;
    map   = p[1];
    row   = p[2];
    state = p[3];
    if (state == 2)
    {
        Cut_PassedAdd(from, map, row);
        return;
    }
    if (state != 1 || map != Cut_MyMap()) return;
    if (from == s_gateToldBy && map == s_gateToldMap && row == s_gateToldRow && now - s_gateToldMs < 30000u)
    {
        return;
    }
    s_gateToldBy = from; s_gateToldMap = map; s_gateToldRow = row; s_gateToldMs = now;
    {
        char line[96];
        snprintf(line, sizeof(line), "%s is waiting for you at a gate", Cut_NameOf(from));
        ShNet_PostLocalEvent(line);
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch hooks                                                      */
/* ------------------------------------------------------------------ */

void ShNetCut_OnDispatch(void)
{
    const unsigned int now = ShNetPlat_Millis();
    int                row;

    if (!Cut_Coop() || g_MapEventData == NULL || g_MapOverlayHdr.mapEvents == NULL)
    {
        return;
    }
    row = (int)(g_MapEventData - g_MapOverlayHdr.mapEvents);
    if (row < 0 || row >= 128)
    {
        return;
    }
    if (g_MapEventSysState != SysState_EventCallback)
    {
        return;
    }
    if (g_MapEventData->triggerType == TriggerType_None)
    {
        /* A map-entry scene: announced now, so partners start with us
         * rather than a confirmation later. */
        Cut_GoLive(row, now);
        s_pendRow = -1;
        return;
    }
    s_pendRow = row;
    s_pendMs  = now;
}

/* A partner who is between worlds (loading a map, or in a movie or a
 * full-screen menu on ours): their pose carries no map, or says so, or has
 * stopped. The first such name, for the screen. */
static int Cut_SomeoneNotReady(char* who, int cap)
{
    const unsigned int now = ShNetPlat_Millis();
    const int          myMap = Cut_MyMap();
    int                i, n = ShSession_MemberCount();

    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        int                    busy;
        if (!m || m->steamId == ShSession_SelfId() || !m->linked)
        {
            continue;
        }
        busy = (m->mapIdx < 0) ||
               (m->mapIdx == myMap && ((m->flags & SHNET_PF_MENU) || m->poseMs == 0 || now - m->poseMs > CUT_POSE_FRESH_MS));
        if (busy)
        {
            if (who) snprintf(who, (size_t)cap, "%s", m->name[0] ? m->name : "a partner");
            return 1;
        }
    }
    return 0;
}

/* A guest's map-entry scene waits for the host's announcement, so both
 * start on the host's frame, as long as the host is here or on the way. */
static int Cut_WaitForHost(char* who, int cap)
{
    const ShSessionMember* h;

    if (ShSession_IsHost()) return 0;
    if (s_joinRow >= 0)
    {
        if (who) snprintf(who, (size_t)cap, "%s", s_joinWho);
        return 1; /* the announcement is here; the join starts it */
    }
    h = Cut_MemberById(ShSession_HostId());
    if (!h || !h->linked || h->steamId == ShSession_SelfId()) return 0;
    if (h->mapIdx >= 0 && h->mapIdx != Cut_MyMap()) return 0;
    if (who) snprintf(who, (size_t)cap, "%s", h->name[0] ? h->name : "the host");
    return 1;
}

int ShNetCut_HoldDispatch(void)
{
    const unsigned int now = ShNetPlat_Millis();
    int                row, map;

    if (s_gateInject)
    {
        s_gateInject = 0;
        return 0;
    }
    if (!Cut_Coop() || g_MapEventData == NULL || g_MapOverlayHdr.mapEvents == NULL ||
        g_GameWork.gameState != GameState_InGame)
    {
        return 0;
    }
    row = (int)(g_MapEventData - g_MapOverlayHdr.mapEvents);
    map = Cut_MyMap();
    if (s_gateRow != NULL && g_MapEventData != s_gateRow)
    {
        g_MapEventSysState = SysState_Invalid; /* nothing else while we wait at a door */
        return 1;
    }
    if ((g_MapEventSysState == SysState_LoadRoom || g_MapEventSysState == SysState_LoadOverlay) &&
        Cut_IsGate(map, row))
    {
        return Cut_GateHold(map, row, now);
    }
    if (g_MapEventSysState != SysState_EventCallback || g_MapEventData->triggerType != TriggerType_None)
    {
        return 0;
    }
    if (row != s_holdRow)
    {
        s_holdRow   = row;
        s_holdSince = now;
    }
    if (now - s_holdSince > CUT_HOLD_MS)
    {
        return 0;
    }
    if (!Cut_SomeoneNotReady(s_holdWho, (int)sizeof(s_holdWho)) && !Cut_WaitForHost(s_holdWho, (int)sizeof(s_holdWho)))
    {
        return 0;
    }
    if (!s_holdFroze)
    {
        s_holdFroze = 1;
        Cut_Freeze();
        SH_DBG("[CUTSCENE] holding map %d row %d for %s", map, row, s_holdWho);
    }
    s_heldThisFrame    = 1;
    g_MapEventSysState = SysState_Invalid; /* Event_Update offers the row again next frame */
    return 1;
}

static void Cut_Unhold(void)
{
    if (s_holdFroze)
    {
        s_holdFroze = 0;
        Cut_Thaw();
        SH_DBG("[CUTSCENE] hold released");
    }
    s_holdRow = -1;
}

static void Cut_Join(void)
{
    const s_EventData* row;
    int                n = Cut_RowCount();

    if (s_joinRow < 0 || s_joinRow >= n || Cut_MyMap() != s_joinMap ||
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
        Cut_GateEnd();
        s_heldThisFrame = 0;
        s_gateInject    = 0;
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
    s_gateInject    = 0;

    /* Sender: the dispatched row became a scene (or did not). */
    if (s_pendRow >= 0)
    {
        if (Cut_SceneLive() && g_GameWork.gameState == GameState_InGame)
        {
            Cut_GoLive(s_pendRow, now);
            s_pendRow = -1;
        }
        else if (now - s_pendMs > CUT_CONFIRM_MS)
        {
            s_pendRow = -1; /* an item take, a message: not a scene */
        }
    }

    /* While the scene runs, repeat it for whoever arrives late. */
    if (s_liveRow >= 0)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_liveMap ||
            (!Cut_SceneLive() && now - s_liveSince > CUT_CONFIRM_MS))
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
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_joinMap ||
            Cut_SceneLive() || g_SysWork.playerWork.player.health <= 0 ||
            now - s_joinMs > CUT_JOIN_WAIT_MS)
        {
            /* Left the map, already in a scene of our own, dead, or a menu
             * stayed up too long. */
            s_joinRow = -1;
        }
        else if (g_SysWork.sysState == SysState_Gameplay && s_gateRow == NULL)
        {
            Cut_Join();
        }
    }

    /* The story door we wait at. */
    if (s_gateRow != NULL)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_gateMap)
        {
            Cut_GateEnd();
        }
        else if (s_gateGo)
        {
            if (now - s_gateSince > CUT_GATE_GO_MS)
            {
                Cut_GateEnd(); /* the dispatch never came (a menu, a scene) */
            }
        }
        else if (!Cut_GateWaitingOn(s_gateMap, s_gateIdx, NULL, 0) || now - s_gateSince > CUT_GATE_WAIT_MS)
        {
            SH_DBG("[CUTSCENE] map %d row %d: %s", s_gateMap, s_gateIdx,
                   (now - s_gateSince > CUT_GATE_WAIT_MS) ? "gave up waiting" : "everyone is here");
            s_gateGo    = 1;
            s_gateSince = now;
            s_gateFroze = 0;
            Cut_Thaw();
        }
    }

    /* The alley death, once we are back in plain gameplay. */
    if (s_dieReq)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != MapIdx_MAP0_S00 ||
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
    if (g_SavegamePtr == NULL || map != Cut_MyMap() || row >= Cut_RowCount() ||
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

    if (!Cut_Coop() || g_SavegamePtr == NULL || Cut_MyMap() != MapIdx_MAP0_S00)
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
    if (p[1] != MapIdx_MAP0_S00 || Cut_MyMap() != MapIdx_MAP0_S00)
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

/* ------------------------------------------------------------------ */
/* Seats                                                               */
/* ------------------------------------------------------------------ */

/* The host has seat 0; guests take the rest in order of id, counted over
 * every linked member so a seat never changes under a player mid-scene. */
static int Cut_MySeat(void)
{
    const unsigned long long me   = ShSession_SelfId();
    const unsigned long long host = ShSession_HostId();
    int                      i, n = ShSession_MemberCount(), seat = 1;

    if (ShSession_IsHost() || me == host) return 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || !m->linked || m->steamId == me || m->steamId == host) continue;
        if (m->steamId < me) seat++;
    }
    return seat;
}

static const CutSeat* Cut_SeatTable(void)
{
    if (s_liveMap == MapIdx_MAP0_S01 && s_liveRow == 3) return s_seatsCafe;
    return s_seatsDefault;
}

int ShNetCut_SelfOffset(int* ox, int* oz, int* turn)
{
    const CutSeat* t;
    int            seat, fwd, side;
    short          rot;

    *ox = *oz = *turn = 0;
    if (!Cut_Coop() || g_GameWork.gameState != GameState_InGame || s_liveRow < 0 || !Cut_SceneLive())
    {
        return 0;
    }
    seat = Cut_MySeat();
    if (seat <= 0 || seat >= CUT_SEATS)
    {
        return 0;
    }
    t    = &Cut_SeatTable()[seat];
    rot  = g_SysWork.playerWork.player.rotation.vy;
    fwd  = (t->fwd * 4096) / 10;
    side = (t->side * 4096) / 10;
    *ox   = Q12_MULT(Math_Sin(rot), fwd) + Q12_MULT(Math_Cos(rot), side);
    *oz   = Q12_MULT(Math_Cos(rot), fwd) - Q12_MULT(Math_Sin(rot), side);
    *turn = (t->turn * 4096) / 360;
    return 1;
}

void ShNetCut_SelfOffsetApply(void* coordsIn)
{
    GsCOORDINATE2* coords = (GsCOORDINATE2*)coordsIn;
    int            ox, oz, turn;

    if (coords == NULL || !ShNetCut_SelfOffset(&ox, &oz, &turn))
    {
        return;
    }
    coords->coord.t[0] += Q12_TO_Q8(ox);
    coords->coord.t[2] += Q12_TO_Q8(oz);
    if (turn != 0)
    {
        /* The matrix was just rebuilt from the rotation this frame (both the
         * gameplay and the scene path do, before the position copy), so
         * rebuilding it again with the turn is exact and repeatable. */
        SVECTOR r;
        r.vx = (short)g_SysWork.playerWork.player.rotation.vx;
        r.vy = (short)(g_SysWork.playerWork.player.rotation.vy + turn);
        r.vz = (short)g_SysWork.playerWork.player.rotation.vz;
        r.pad = 0;
        Math_RotMatrixZxyNegGte(&r, &coords->coord);
    }
    coords->flg = false;
}

int ShNetCut_StatusText(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return 0;
    }
    if (s_gateRow != NULL && !s_gateGo)
    {
        snprintf(out, (size_t)cap, "Waiting for other players...");
        return 1;
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
