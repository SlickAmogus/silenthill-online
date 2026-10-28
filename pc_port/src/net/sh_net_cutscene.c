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
 * camera themselves. A player who joins the game mid-scene hears the
 * repeats and plays it too. A scene is joined once: the rows played on the
 * current map are remembered, so a repeat never pulls anyone back.
 *
 * "In a scene" is the handler's state (SysState_EventCallback, or the movie
 * inside it), never SysFlag_CutsceneActive: the Cheryl chase keeps that flag
 * up for minutes, and reading it as "still in the scene" kept re-announcing
 * a scene long over and pulled the partner back to it.
 *
 * Map-entry scenes (TriggerType_None rows, which fire the moment a map is
 * up) start together: the host holds its own back while a partner is still
 * loading the map or sitting in a movie or a menu, announces the row the
 * frame it dispatches, and a guest holds its own copy of the row until that
 * announcement comes (or the host turns out to be elsewhere). On one PC the
 * two halves then start within a frame of each other; over the network,
 * within the one-way latency.
 *
 * Scenes end together too: the first player out of a scene (finished or
 * skipped) waits on a black "Waiting for cutscene..." screen until no
 * partner on the map is still in it or about to join it, which every pose
 * says (sceneRow / sceneState), so a lost message cannot strand anyone.
 *
 * Story doors (s_gates below: a door or gate with a scene, or a point of no
 * return, behind it) belong to the host. A guest who uses one stands at it
 * under "Waiting for other players..." until the host goes through. The host
 * goes through once every partner on the map who has not been through it is
 * close by (or at once with Action), and every such partner goes through
 * with them in the same moment. Two machines deciding on their own would let
 * one slip through while the other is a step short, and the one left behind
 * may then find the door shut by the flags the first one sent.
 * Cancel turns back.
 *
 * Seats: a scripted scene moves every player along the same path, so they
 * would stand inside each other. Each player's game offsets its OWN model
 * by its seat (the host sits where the script put it, guests a step aside;
 * a scene can have its own table), for the drawing and for the pose it
 * sends, so every screen shows the same arrangement. The simulated position
 * is never touched.
 *
 * The identifier on the wire is (map, row): eventParam alone repeats across
 * rows, and a disabledEventFlag arriving early through the flag sync would
 * make the ordinary trigger path skip the row forever, which is why the
 * start is forced rather than simulated.
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

#define CUT_CONFIRM_MS    1000u   /* a dispatched row has this long to become a scene */
#define CUT_JOIN_WAIT_MS  12000u  /* a join waits this long after the last repeat for a menu or movie to end */
#define CUT_REANNOUNCE_MS 2000u
#define CUT_HOLD_MS       20000u  /* a map-entry scene waits this long for partners */
#define CUT_END_WAIT_MS   120000u /* the end of a scene waits this long for the others */
#define CUT_DIE_WAIT_MS   10000u
#define CUT_GATE_WAIT_MS  90000u  /* a story door waits this long for partners */
#define CUT_GATE_GO_MS    5000u   /* a released door has this long to dispatch */
#define CUT_FOLLOW_MS     20000u  /* the host's "come with me" keeps this long */
#define CUT_GATE_NEAR     Q12(7.0f)
#define CUT_STRAND_MS     30000u  /* a scene step this long unchanged, with everyone else out: stuck */
#define CUT_STRAND_HARD_MS 5000u  /* after the script's own ending was asked for */
#define CUT_POSE_FRESH_MS 2000u   /* a partner whose pose is older than this is not ready, and not waited for */
#define CUT_PRESS_GRACE_MS 500u   /* the press that opened a door wait is not a second use */

/* Story doors: rows (LoadRoom / LoadOverlay) that wait for the partners who
 * have not been through them. Only doors with a scene or a point of no return
 * behind them belong here; an ordinary door is walked through alone. */
typedef struct { unsigned char map, row; } CutGate;
static const CutGate s_gates[] = {
    { MapIdx_MAP0_S00, 15 }, /* the alley's first gate: alley2 and its own cameras are past it */
    { MapIdx_MAP0_S00, 17 }, /* the second gate: the dark, the lighter and the end of the alley */
    { MapIdx_MAP2_S00, 72 }, /* the school's Door of Eclipse: the otherworld is behind it */
};

/* A story door (s_gates) waits for everyone, and the host may bring them
 * through. Any other change of map (a LoadOverlay row) is a level door: a
 * guest may not use one while the host is on this map and not beside them,
 * so nobody moves on to a new level without the host; a host who went on
 * ahead is simply followed. The host walks through level doors freely. */
#define DOOR_STORY 1
#define DOOR_LEVEL 2

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

/* Rows played on the current map: never joined again. */
static unsigned int s_played[4];
static int          s_playedMap = -1;

/* Receiver side: a scene to join. */
static int          s_joinRow = -1;
static int          s_joinMap = -1;
static VECTOR3      s_joinPos;
static short        s_joinRot;
static char         s_joinWho[SHSESSION_NAME_MAX];
static unsigned int s_joinMs;

/* The end of a scene, waiting for the others. */
static int          s_waitMap = -1, s_waitRow = -1;
static unsigned int s_waitSince;
static int          s_waitFroze;

/* The player stands still for a map-entry hold, a door, or the end wait. */
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
static int          s_gateFroze, s_gateGo, s_gateInject, s_gateFollow;
static int          s_gateKind;
static unsigned int s_gateGoMs;
static unsigned int s_gatePrevHeld;
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

/* A scene handler has the state (a movie inside it included). */
static int Cut_HandlerRunning(void)
{
    return g_SysWork.sysState == SysState_EventCallback || g_SysWork.sysState == SysState_Fmv;
}

/* We are inside the scene we started or joined. */
static int Cut_InMyScene(void)
{
    return s_liveRow >= 0 && Cut_HandlerRunning();
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

static int Cut_PoseFresh(const ShSessionMember* m, unsigned int now)
{
    return m->poseMs != 0 && now - m->poseMs < CUT_POSE_FRESH_MS;
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

/* Thaws once nothing needs the player still. */
static void Cut_Thaw(void)
{
    if (s_frozen && !s_holdFroze && !s_gateFroze && !s_waitFroze)
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

static int Cut_Played(int map, int row)
{
    return map == s_playedMap && row >= 0 && row < 128 && (s_played[row >> 5] & (1u << (row & 31)));
}

static void Cut_PlayedAdd(int map, int row)
{
    if (map != s_playedMap)
    {
        memset(s_played, 0, sizeof(s_played));
        s_playedMap = map;
    }
    if (row >= 0 && row < 128) s_played[row >> 5] |= 1u << (row & 31);
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
    Cut_PlayedAdd(s_liveMap, row);
    SH_DBG("[CUTSCENE] started map %d row %d; told the room", s_liveMap, s_liveRow);
}

/* ------------------------------------------------------------------ */
/* Seats                                                               */
/* ------------------------------------------------------------------ */

/* The host has seat 0; guests take the rest in order of id, counted over
 * every linked member so a seat never changes under a player mid-scene. */
static int Cut_SeatOf(unsigned long long id)
{
    const unsigned long long host = ShSession_HostId();
    int                      i, n = ShSession_MemberCount(), seat = 1;

    if (id == host) return 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || !m->linked || m->steamId == id || m->steamId == host) continue;
        if (m->steamId < id) seat++;
    }
    return seat;
}

static const CutSeat* Cut_SeatTable(void)
{
    if (s_liveMap == MapIdx_MAP0_S01 && s_liveRow == 3) return s_seatsCafe;
    return s_seatsDefault;
}

int ShNetCut_SelfSeat(void)
{
    return Cut_Coop() ? Cut_SeatOf(ShSession_SelfId()) : 0;
}

int ShNetCut_SelfOffset(int* ox, int* oz, int* turn)
{
    const CutSeat* t;
    int            seat, fwd, side;
    short          rot;

    *ox = *oz = *turn = 0;
    if (!Cut_Coop() || g_GameWork.gameState != GameState_InGame || !Cut_InMyScene())
    {
        return 0;
    }
    seat = Cut_SeatOf(ShSession_SelfId());
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

/* ------------------------------------------------------------------ */
/* Scene state in the pose, and the end of a scene                    */
/* ------------------------------------------------------------------ */

int ShNetCut_PoseScene(int* row)
{
    *row = 0xFF;
    if (!Cut_Coop() || g_GameWork.gameState != GameState_InGame) return 0;
    if (Cut_InMyScene())
    {
        *row = s_liveRow;
        return 2;
    }
    if (s_joinRow >= 0 && s_joinMap == Cut_MyMap())
    {
        *row = s_joinRow;
        return 1;
    }
    if (s_holdFroze && s_holdRow >= 0)
    {
        *row = s_holdRow;
        return 1;
    }
    return 0;
}

/* A partner on this map whose pose says they are in, or about to be in,
 * the scene (map, row). */
static int Cut_PeerStillIn(int map, int row, unsigned int now)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || !m->linked || m->steamId == ShSession_SelfId()) continue;
        if (m->mapIdx != map || !Cut_PoseFresh(m, now)) continue;
        if (m->sceneRow == row && m->sceneState != 0) return 1;
    }
    return 0;
}

static void Cut_WaitEnd(const char* why)
{
    if (s_waitRow >= 0)
    {
        SH_DBG("[CUTSCENE] end of map %d row %d: %s", s_waitMap, s_waitRow, why);
    }
    s_waitRow   = -1;
    s_waitMap   = -1;
    s_waitFroze = 0;
    Cut_Thaw();
}

/* Our scene is over. */
static void Cut_SceneEnded(int map, int row, unsigned int now)
{
    if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != map || !Cut_PeerStillIn(map, row, now))
    {
        return;
    }
    s_waitMap   = map;
    s_waitRow   = row;
    s_waitSince = now;
    s_waitFroze = 1;
    Cut_Freeze();
    SH_DBG("[CUTSCENE] out of map %d row %d first; waiting for the others", map, row);
}

int ShNetCut_WaitActive(void)
{
    return s_waitRow >= 0;
}

/* The players on this map, host first: seat label, name, and whether they
 * are out of the scene being waited for. */
int ShNetCut_WaitEntry(int i, char* out, int cap, int* ready)
{
    const unsigned int now = ShNetPlat_Millis();
    int k, n = ShSession_MemberCount(), idx = 0, best = -1;
    unsigned long long me = ShSession_SelfId();

    if (s_waitRow < 0 || !out || cap <= 0) return 0;
    /* Walk the members in seat order: the k-th smallest seat. */
    for (;;)
    {
        int pick = -1, pickSeat = 1 << 30;
        for (k = 0; k < n; k++)
        {
            const ShSessionMember* m = ShSession_Member(k);
            int                    seat;
            if (!m || !m->linked) continue;
            if (m->steamId != me && m->mapIdx != s_waitMap) continue;
            seat = Cut_SeatOf(m->steamId);
            if (seat > best && seat < pickSeat)
            {
                pick     = k;
                pickSeat = seat;
            }
        }
        if (pick < 0) return 0;
        best = pickSeat;
        if (idx == i)
        {
            const ShSessionMember* m = ShSession_Member(pick);
            snprintf(out, (size_t)cap, "P%d  %s", pickSeat + 1, m->name[0] ? m->name : "Player");
            if (ready)
            {
                *ready = (m->steamId == me) ? 1
                       : !(Cut_PoseFresh(m, now) && m->sceneRow == s_waitRow && m->sceneState != 0);
            }
            return 1;
        }
        idx++;
    }
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

/* state 1: waiting at it; 2: went through; 3: host, come through with me. */
static void Cut_GateTell(int map, int row, int state)
{
    unsigned char b[4];
    int           r;
    b[0] = SHNET_CTRL_GATE;
    b[1] = (unsigned char)map;
    b[2] = (unsigned char)row;
    b[3] = (unsigned char)state;
    for (r = 0; r < (state == 1 ? 1 : 2); r++)
    {
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, 4, 0, 1);
    }
}

/* A partner on this map who has not been through the door and is not beside
 * us (or is still loading the map). A guest waits on the host alone: beside
 * them for a level door, through it for a story door. The first such name,
 * for the log. */
static int Cut_GateWaitingOn(int map, int row, int kind, char* who, int cap)
{
    const s_SubCharacter* me       = &g_SysWork.playerWork.player;
    const int             hostOnly = !ShSession_IsHost();
    int                   i, n = ShSession_MemberCount();

    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        int                    far;
        if (!m || m->steamId == ShSession_SelfId() || !m->linked) continue;
        if (hostOnly && m->steamId != ShSession_HostId()) continue;
        if (Cut_Passed(m->steamId, map, row)) continue;
        if (m->mapIdx >= 0 && m->mapIdx != map) continue; /* elsewhere: cannot come */
        if (m->healthPct1 == 1) continue;                  /* dead */
        far = (m->mapIdx < 0 || m->poseMs == 0 || (hostOnly && kind == DOOR_STORY) ||
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
    s_gateRow    = NULL;
    s_gateGo     = 0;
    s_gateFollow = 0;
    s_gateFroze  = 0;
    Cut_Thaw();
}

/* Through we go: the held row is dispatched by ShNetCut_GateInject. */
static void Cut_GateRelease(unsigned int now, const char* why)
{
    SH_DBG("[CUTSCENE] map %d row %d: %s", s_gateMap, s_gateIdx, why);
    s_gateGo    = 1;
    s_gateGoMs  = now;
    s_gateFroze = 0;
    Cut_Thaw();
}

/* The door's row is about to dispatch. */
static int Cut_GateHold(int map, int row, int kind, unsigned int now)
{
    char who[SHSESSION_NAME_MAX];

    if (s_gateRow == g_MapEventData)
    {
        g_MapEventSysState = SysState_Invalid; /* the buttons are read by the tick */
        return 1;
    }
    if (!Cut_GateWaitingOn(map, row, kind, who, (int)sizeof(who)))
    {
        Cut_PassedAdd(ShSession_SelfId(), map, row);
        if (kind == DOOR_STORY)
        {
            if (ShSession_IsHost()) Cut_GateTell(map, row, 3);
            Cut_GateTell(map, row, 2);
        }
        return 0;
    }
    s_gateRow      = g_MapEventData;
    s_gateMap      = map;
    s_gateIdx      = row;
    s_gateKind     = kind;
    s_gateSince    = now;
    s_gateFroze    = 1;
    s_gateFollow   = 0;
    s_gatePrevHeld = (unsigned int)g_Controller0->heldBtnFlags;
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
    Cut_PassedAdd(ShSession_SelfId(), s_gateMap, s_gateIdx);
    Cut_GateTell(s_gateMap, s_gateIdx, 2);
    SH_DBG("[CUTSCENE] going through map %d row %d%s", s_gateMap, s_gateIdx, s_gateFollow ? " with the host" : "");
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
    if (map != Cut_MyMap()) return;
    if (state == 3)
    {
        /* The host takes everyone through with them. Only the host may, only
         * a door row, and not twice. */
        if (from != ShSession_HostId() || row >= Cut_RowCount() || Cut_Passed(ShSession_SelfId(), map, row)) return;
        if (g_MapOverlayHdr.mapEvents[row].sysState != SysState_LoadRoom &&
            g_MapOverlayHdr.mapEvents[row].sysState != SysState_LoadOverlay)
        {
            return;
        }
        Cut_GateEnd();
        s_gateRow    = &g_MapOverlayHdr.mapEvents[row];
        s_gateMap    = map;
        s_gateIdx    = row;
        s_gateFollow = 1;
        s_gateGo     = 1;
        s_gateGoMs   = now;
        {
            char line[96];
            snprintf(line, sizeof(line), "%s brings everyone through", Cut_NameOf(from));
            ShNet_PostLocalEvent(line);
        }
        SH_DBG("[CUTSCENE] %s brings everyone through map %d row %d", Cut_NameOf(from), map, row);
        return;
    }
    if (state != 1) return;
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

/* The door wait, each frame: arrival, the buttons, the give-up. */
static void Cut_GateTick(unsigned int now)
{
    const unsigned int held  = (unsigned int)g_Controller0->heldBtnFlags;
    const unsigned int press = held & ~s_gatePrevHeld;
    s_gatePrevHeld = held;

    if (s_gateRow == NULL) return;
    if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_gateMap)
    {
        Cut_GateEnd();
        return;
    }
    if (s_gateGo)
    {
        if (now - s_gateGoMs > (s_gateFollow ? CUT_FOLLOW_MS : CUT_GATE_GO_MS))
        {
            Cut_GateEnd(); /* the dispatch never came (a menu, a scene) */
        }
        return;
    }
    if (now - s_gateSince > CUT_PRESS_GRACE_MS)
    {
        if (press & g_GameWorkPtr->config.controllerConfig.cancel)
        {
            SH_DBG("[CUTSCENE] map %d row %d: turned back", s_gateMap, s_gateIdx);
            Cut_GateEnd();
            return;
        }
        if ((press & g_GameWorkPtr->config.controllerConfig.action) && ShSession_IsHost() && s_gateKind == DOOR_STORY)
        {
            Cut_GateTell(s_gateMap, s_gateIdx, 3);
            Cut_GateRelease(now, "bringing everyone through");
            return;
        }
    }
    if (!Cut_GateWaitingOn(s_gateMap, s_gateIdx, s_gateKind, NULL, 0))
    {
        if (ShSession_IsHost() && s_gateKind == DOOR_STORY) Cut_GateTell(s_gateMap, s_gateIdx, 3);
        Cut_GateRelease(now, "everyone is here");
    }
    else if (now - s_gateSince > CUT_GATE_WAIT_MS)
    {
        if (ShSession_IsHost())
        {
            if (s_gateKind == DOOR_STORY) Cut_GateTell(s_gateMap, s_gateIdx, 3);
            Cut_GateRelease(now, "gave up waiting; bringing everyone");
        }
        else
        {
            /* A guest never goes on alone. */
            SH_DBG("[CUTSCENE] map %d row %d: the others did not come; turned back", s_gateMap, s_gateIdx);
            ShNet_PostLocalEvent("The others did not come; the door stays shut");
            Cut_GateEnd();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bringing a player to another                                        */
/* ------------------------------------------------------------------ */

/* To a spot on this map at once; to another map through a reboot into it
 * that keeps the inventory and flags (pc_coop_menu.c). */
static void Cut_GoTo(int map, int x, int y, int z, short rot, const char* who)
{
    extern void Pc_CoopMenu_RequestWarp(int mapIdx, int x, int z, short rotY);
    char line[96];

    if (g_GameWork.gameState != GameState_InGame) return;
    if (map == Cut_MyMap())
    {
        VECTOR3 pos;
        if (Cut_HandlerRunning()) return; /* mid-scene: the script owns our place */
        pos.vx = x; pos.vy = y; pos.vz = z;
        ShNetRespawn_PlaceAt((const void*)&pos, rot);
        snprintf(line, sizeof(line), "Brought to %s", who);
        ShNet_PostLocalEvent(line);
        SH_DBG("[CUTSCENE] brought to %s on map %d", who, map);
        return;
    }
    SH_DBG("[CUTSCENE] going to %s's map %d", who, map);
    Pc_CoopMenu_RequestWarp(map, x, z, rot);
}

void ShNetCut_Summon(unsigned long long id)
{
    const s_SubCharacter* p = &g_SysWork.playerWork.player;
    unsigned char         b[32];
    int                   o = 0, r;

    if (!Cut_Coop() || !ShSession_IsHost() || g_GameWork.gameState != GameState_InGame || id == ShSession_SelfId())
    {
        return;
    }
    b[o++] = SHNET_CTRL_SUMMON;
    ShnPutU64(b, &o, id);
    b[o++] = (unsigned char)Cut_MyMap();
    ShnPutS32(b, &o, p->position.vx);
    ShnPutS32(b, &o, p->position.vy);
    ShnPutS32(b, &o, p->position.vz);
    ShnPutS16(b, &o, p->rotation.vy);
    for (r = 0; r < 2; r++)
    {
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, o, 0, 1);
    }
    SH_DBG("[CUTSCENE] bringing %s here (map %d)", Cut_NameOf(id), Cut_MyMap());
}

void ShNetCut_SummonIn(unsigned long long from, const unsigned char* p, int len)
{
    static unsigned int s_lastMs;
    const unsigned int  now = ShNetPlat_Millis();
    int                 o = 1, map;
    unsigned long long  target;
    VECTOR3             pos;
    short               rot;

    if (len < 24 || !Cut_Coop() || from != ShSession_HostId() || from == ShSession_SelfId()) return;
    target = ShnGetU64(p, &o);
    if (target != ShSession_SelfId()) return;
    map    = p[o++];
    pos.vx = ShnGetS32(p, &o);
    pos.vy = ShnGetS32(p, &o);
    pos.vz = ShnGetS32(p, &o);
    rot    = (short)ShnGetS16(p, &o);
    if (map > MapIdx_MAPX_S00 ||
        pos.vx > SHNET_COORD_MAX || pos.vx < -SHNET_COORD_MAX || pos.vy > SHNET_COORD_MAX || pos.vy < -SHNET_COORD_MAX ||
        pos.vz > SHNET_COORD_MAX || pos.vz < -SHNET_COORD_MAX)
    {
        return;
    }
    if (s_lastMs && now - s_lastMs < 1000u) return; /* sent twice */
    s_lastMs = now;
    Cut_GoTo(map, pos.vx, pos.vy, pos.vz, rot, Cut_NameOf(from));
}

void ShNetCut_TeleportToHost(void)
{
    const ShSessionMember* h;

    if (!Cut_Coop() || ShSession_IsHost()) return;
    h = Cut_MemberById(ShSession_HostId());
    if (!h || !h->linked || h->mapIdx < 0 || h->poseMs == 0)
    {
        ShNet_PostLocalEvent("The host is between maps; try again in a moment");
        return;
    }
    Cut_GoTo(h->mapIdx, h->x, h->y, h->z, h->rotY, h->name[0] ? h->name : "the host");
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
               (m->mapIdx == myMap && ((m->flags & SHNET_PF_MENU) || !Cut_PoseFresh(m, now)));
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
    /* Nothing else while we wait at a door, for the others, or for a
     * map-entry scene (standing in a trigger's box while held must not
     * start that trigger instead). */
    if ((s_gateRow != NULL && g_MapEventData != s_gateRow) || s_waitRow >= 0 ||
        (s_holdFroze && row != s_holdRow))
    {
        g_MapEventSysState = SysState_Invalid;
        return 1;
    }
    if (g_MapEventSysState == SysState_LoadRoom || g_MapEventSysState == SysState_LoadOverlay)
    {
        const int kind = Cut_IsGate(map, row) ? DOOR_STORY
                       : (g_MapEventSysState == SysState_LoadOverlay && !ShSession_IsHost()) ? DOOR_LEVEL : 0;
        if (kind)
        {
            return Cut_GateHold(map, row, kind, now);
        }
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

static void Cut_Join(unsigned int now)
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
    /* Ours now too: the seat, the repeats for later arrivals, the end wait. */
    Cut_GoLive(s_joinRow, now);
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

/* A joined scene can wait on state only its starter had: the alley's
 * "getting darker" step waits for a darkness value that grows from the
 * player's own walk down the alley, and a partner placed at the scene's start
 * never walked it. Nobody can leave such a scene. When everyone else on the
 * map is out of it and our script has sat on one step for CUT_STRAND_MS, ask
 * the script for its own ending (each handler's default case fades back,
 * unfreezes and returns to gameplay); if even that does not end it, return to
 * gameplay directly. The flags the scene would have set arrive from the
 * partners through the flag sync. */
static int          s_strandStep = -1;
static unsigned int s_strandMs;
static unsigned int s_strandKickMs;

static int Cut_PeerOnMap(int map, unsigned int now)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->linked && m->steamId != ShSession_SelfId() && m->mapIdx == map && Cut_PoseFresh(m, now)) return 1;
    }
    return 0;
}

static void Cut_StrandCheck(unsigned int now)
{
    const int step = g_SysWork.sysStateSteps[0];

    if (s_liveRow < 0 || !Cut_InMyScene() || g_SysWork.sysState != SysState_EventCallback ||
        !Cut_PeerOnMap(s_liveMap, now) || Cut_PeerStillIn(s_liveMap, s_liveRow, now))
    {
        s_strandMs     = 0;
        s_strandKickMs = 0;
        return;
    }
    if (s_strandKickMs != 0)
    {
        if (now - s_strandKickMs > CUT_STRAND_HARD_MS)
        {
            SH_DBG("[CUTSCENE] map %d row %d still stuck after its ending; back to gameplay", s_liveMap, s_liveRow);
            SysWork_StateSetNext(SysState_Gameplay);
            g_MapOverlayHdr.playerControlUnfreeze(true);
            s_strandKickMs = 0;
            s_strandMs     = 0;
        }
        return;
    }
    if (s_strandMs == 0 || step != s_strandStep)
    {
        s_strandMs   = now;
        s_strandStep = step;
        return;
    }
    if (now - s_strandMs > CUT_STRAND_MS)
    {
        SH_DBG("[CUTSCENE] map %d row %d stuck on step %d with everyone else out; ending it", s_liveMap, s_liveRow, step);
        ShNet_PostLocalEvent("Scene stuck; ending it");
        g_SysWork.sysStateSteps[0] = 99;
        g_SysWork.sysStateSteps[1] = 0;
        g_SysWork.sysStateSteps[2] = 0;
        s_strandKickMs = now;
    }
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
        s_playedMap = -1;
        Cut_Unhold();
        Cut_GateEnd();
        Cut_WaitEnd("no game");
        s_heldThisFrame = 0;
        s_gateInject    = 0;
        return;
    }
    if (Cut_MyMap() != s_playedMap)
    {
        memset(s_played, 0, sizeof(s_played));
        s_playedMap = Cut_MyMap();
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
        if (Cut_SceneLive() && Cut_HandlerRunning() && g_GameWork.gameState == GameState_InGame)
        {
            Cut_GoLive(s_pendRow, now);
            s_pendRow = -1;
        }
        else if (now - s_pendMs > CUT_CONFIRM_MS || !Cut_HandlerRunning())
        {
            s_pendRow = -1; /* an item take, a message: not a scene */
        }
    }

    /* While the scene runs, repeat it for whoever arrives late; when it is
     * over, wait for the others to be out of it. */
    if (s_liveRow >= 0)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_liveMap)
        {
            s_liveRow = -1;
        }
        else if (!Cut_HandlerRunning() && now - s_liveSince > CUT_CONFIRM_MS)
        {
            const int map = s_liveMap, row = s_liveRow;
            s_liveRow = -1;
            Cut_SceneEnded(map, row, now);
        }
        else if (now - s_liveSentMs >= CUT_REANNOUNCE_MS)
        {
            Cut_Announce(s_liveMap, s_liveRow, &s_livePos, s_liveRot);
            s_liveSentMs = now;
        }
    }

    /* The end of a scene: everyone out, or long enough. */
    if (s_waitRow >= 0)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_waitMap)
        {
            Cut_WaitEnd("left the map");
        }
        else if (!Cut_PeerStillIn(s_waitMap, s_waitRow, now))
        {
            Cut_WaitEnd("everyone is out");
        }
        else if (now - s_waitSince > CUT_END_WAIT_MS)
        {
            Cut_WaitEnd("gave up waiting");
        }
    }

    /* Receiver: join as soon as plain gameplay allows. */
    if (s_joinRow >= 0)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != s_joinMap ||
            Cut_InMyScene() || g_SysWork.playerWork.player.health <= 0 ||
            now - s_joinMs > CUT_JOIN_WAIT_MS)
        {
            /* Left the map, in a scene of our own, dead, or the repeats
             * stopped and a menu stayed up too long. */
            s_joinRow = -1;
        }
        else if (g_SysWork.sysState == SysState_Gameplay && s_gateRow == NULL && s_waitRow < 0)
        {
            Cut_Join(now);
        }
    }

    Cut_StrandCheck(now);
    Cut_GateTick(now);

    /* The alley death, once we are back in plain gameplay. */
    if (s_dieReq)
    {
        if (g_GameWork.gameState != GameState_InGame || Cut_MyMap() != MapIdx_MAP0_S00 ||
            g_SysWork.playerWork.player.health <= 0 || now - s_dieMs > CUT_DIE_WAIT_MS)
        {
            s_dieReq = 0;
        }
        else if (g_SysWork.sysState == SysState_Gameplay && !Cut_HandlerRunning())
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
    if (Cut_Played(map, row))
    {
        return; /* ours already, or joined before: a repeat */
    }
    if (s_joinRow == row && s_joinMap == map)
    {
        s_joinMs = now; /* still on: the join keeps waiting for its moment */
        return;
    }
    if (Cut_InMyScene() || s_joinRow >= 0)
    {
        return; /* in one of our own, or joining another: the repeats will find us */
    }
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
    return Cut_Coop() && g_GameWork.gameState == GameState_InGame && Cut_InMyScene();
}

int ShNetCut_StatusText(char* out, int cap, char* hint, int hintCap)
{
    if (!out || cap <= 0)
    {
        return 0;
    }
    if (hint && hintCap > 0) hint[0] = '\0';
    if (s_gateRow != NULL && !s_gateGo)
    {
        if (s_gateKind == DOOR_LEVEL)
        {
            snprintf(out, (size_t)cap, "Waiting for the host...");
            if (hint) snprintf(hint, (size_t)hintCap, "Cancel: turn back");
            return 1;
        }
        {
            char who[SHSESSION_NAME_MAX];
            who[0] = ' ';
            if (Cut_GateWaitingOn(s_gateMap, s_gateIdx, s_gateKind, who, (int)sizeof(who)) && who[0])
            {
                snprintf(out, (size_t)cap, "Waiting for %s at the door...", who);
            }
            else
            {
                snprintf(out, (size_t)cap, "Waiting for other players...");
            }
        }
        if (hint)
        {
            snprintf(hint, (size_t)hintCap, ShSession_IsHost()
                     ? "Action: go through with everyone      Cancel: turn back"
                     : "Cancel: turn back");
        }
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
