/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sh_net_invite.c - inviting a living-world player into your game (see the header). */

#include "sh_net_invite.h"

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "pc_config.h"
#include "pc_coop_menu.h"
#include "pc_coop_save.h" /* Pc_Save_BossActive */
#include "pc_split_screen.h"
#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_social.h"
#include "sh_log.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define INV_EXPIRE_MS    30000u  /* an invite, on both ends */
#define INV_COOLDOWN_MS  120000u /* after a decline, before asking that player again */
#define INV_HOST_WAIT_MS 20000u  /* our room has this long to open for the invites waiting on it */
#define INV_PEND_MS      180000u /* an invite waits this long for us to be hosting */
#define INV_JOIN_WAIT_MS 45000u  /* the guest has this long to be in the room */
#define INV_LOBBY_MS     5000u   /* in the room with no world sent: the host is still in the lobby */
#define INV_LEAVE_MS     4000u   /* leaving our old room before joining the new one */
#define INV_SAVE_WAIT_MS 300000u /* an invite stays open this long while the invited player saves */
#define INV_SAVE_OPEN_MS 5000u   /* the save screen has this long to come up after we asked for it */
#define INV_COOL_MAX     8
#define INV_OUT_MAX      4

/* Invites we sent, each waiting for its answer. */
static struct
{
    unsigned int to;
    char         name[SHNET_NAME_MAX + 8];
    unsigned int ms;
    int          saving; /* they said yes but are in their save screen first */
} s_out[INV_OUT_MAX];

/* Invites waiting for us to be hosting a room on the server: sent when it opens. */
static struct
{
    unsigned int to;
    char         name[SHNET_NAME_MAX + 8];
    int          seats;
    unsigned int ms;
} s_want[INV_OUT_MAX];
static int          s_hostFromHere;   /* the room is opening for "Host From Here" */
static unsigned int s_hostFromHereMs;

/* The invite waiting for our answer, and the one we said yes to. */
static unsigned int s_inFrom;
static char         s_inName[SHNET_NAME_MAX];
static unsigned int s_inMs;
static unsigned int s_acceptFrom;
static char         s_acceptName[SHNET_NAME_MAX];
static unsigned int s_acceptMs;

/* The invite we are saving our own game for, before accepting it. */
static unsigned int s_saveFrom;
static char         s_saveName[SHNET_NAME_MAX];
static unsigned int s_saveMs;
static int          s_savePhase;     /* 1 save screen asked for, 2 in it */
static int          s_saveCancelled; /* the inviter withdrew while we were in there */

/* Joining a room by its code (an invite's JOIN, /join, or Player 2 following
 * Player 1): 4 giving Player 2 a moment to hear where we go, 1 leaving our
 * old room, 2 joining, 3 in it and waiting for the host's world (or, from
 * the menus, done once we are in). */
#define INV_FOLLOW_MS 700u
static int          s_joinStep;
static int          s_joinCode;
static int          s_joinLobby;     /* the host said they are still in the lobby */
static unsigned int s_joinMs;
static char         s_joinName[SHNET_NAME_MAX + 8];

static struct { unsigned int id; unsigned int untilMs; } s_cool[INV_COOL_MAX];

static int Inv_InGame(void)
{
    return g_GameWork.gameState == GameState_InGame && g_SavegamePtr != NULL;
}

static int Inv_AtMenus(void)
{
    return g_GameWork.gameState == GameState_MainMenu;
}

/* Where the game could be saved: accepting makes a save of the host's game on
 * the spot and drops the guest into it, so neither side may be mid boss
 * fight, the one place the quick save refuses too. */
static int Inv_CanSaveHere(void)
{
    return Inv_InGame() && g_SysWork.sysState == SysState_Gameplay && !Pc_Save_BossActive();
}

static int Inv_ServerRoom(void)
{
    return ShSession_Active() && ShSession_LobbyId() != 0 && ShSession_LobbyId() < 100000ull;
}

static int Inv_HostingServerRoom(void)
{
    return Inv_ServerRoom() && ShSession_IsHost();
}

static void Inv_Toast(const char* fmt, const char* name)
{
    char line[128];
    snprintf(line, sizeof(line), fmt, name);
    ShNet_PostLocalEvent(line);
}

static int Inv_CooldownSec(unsigned int id, unsigned int now)
{
    int i;
    for (i = 0; i < INV_COOL_MAX; i++)
    {
        if (s_cool[i].id == id && s_cool[i].id != 0)
        {
            if ((int)(s_cool[i].untilMs - now) > 0) return (int)((s_cool[i].untilMs - now + 999u) / 1000u);
            s_cool[i].id = 0;
        }
    }
    return 0;
}

static void Inv_CooldownAdd(unsigned int id, unsigned int now)
{
    int i, slot = 0;
    for (i = 0; i < INV_COOL_MAX; i++)
    {
        if (s_cool[i].id == id || s_cool[i].id == 0) { slot = i; break; }
        if ((int)(s_cool[i].untilMs - now) <= 0) slot = i;
    }
    s_cool[slot].id      = id;
    s_cool[slot].untilMs = now + INV_COOLDOWN_MS;
}

static int Inv_IsMember(unsigned int id)
{
    int i, n = ShSession_Active() ? ShSession_MemberCount() : 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && (unsigned int)m->steamId == id) return 1;
    }
    return 0;
}

static int Inv_MaxPlayers(void)
{
    int n = g_PcConfig.onlineSteamMaxPlayers;
    if (n < 2) n = 2;
    if (n > 4) n = 4;
    return n;
}

/* Players in our game besides us and our own split-screen Player 2. */
static int Inv_Others(void)
{
    int  i, n = ShSession_Active() ? ShSession_MemberCount() : 0, k = 0;
    char p2[SHNET_NAME_MAX + 8];
    snprintf(p2, sizeof(p2), "%s (1)", g_PcConfig.onlineName);
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || m->steamId == ShSession_SelfId()) continue;
        if (Pc_Split_Role() == 1 && !SDL_strcasecmp(m->name, p2)) continue;
        k++;
    }
    return k;
}

/* Seats our answer needs: two when Player 2 comes along. */
static int Inv_SeatsNeeded(void)
{
    return Pc_Split_Role() == 1 ? 2 : 1;
}

int ShNetInvite_Blocked(void)
{
    return Inv_InGame() && !Inv_CanSaveHere();
}

int ShNetInvite_CanInvite(void)
{
    if (!ShNet_WorldConnected() || !(Inv_InGame() || Inv_AtMenus())) return 0;
    if (!ShSession_Active()) return Inv_InGame();
    /* Hosting a server room with a seat free; a Steam lobby cannot take a
     * living-world player, and a guest is not the one who invites. */
    return Inv_HostingServerRoom() && ShSession_MemberCount() < Inv_MaxPlayers();
}

unsigned int ShNetInvite_Nearest(char* name, int cap, int* outCooldownSec)
{
    const unsigned int now = SDL_GetTicks();
    int                i, n, best = -1;
    long long          bestD = 0;
    const int          mx = g_SysWork.playerWork.player.position.vx;
    const int          mz = g_SysWork.playerWork.player.position.vz;

    if (name && cap > 0) name[0] = '\0';
    if (outCooldownSec) *outCooldownSec = 0;
    if (!Inv_InGame()) return 0;
    n = ShNet_GhostCount();
    for (i = 0; i < n; i++)
    {
        const ShNetGhost* g = ShNet_Ghost(i);
        int       x, y, z;
        short     r;
        long long dx, dz, d;
        if (!g || g->playerId == 0 || g->playerId == ShNet_WorldSelfId() || Inv_IsMember(g->playerId)) continue;
        if (ShSocial_BlockedToken(g->token)) continue;
        if (!ShNet_GhostInterp(i, &x, &y, &z, &r)) { x = g->curX; z = g->curZ; }
        dx = (long long)x - mx;
        dz = (long long)z - mz;
        d  = dx * dx + dz * dz;
        if (best < 0 || d < bestD) { best = i; bestD = d; }
    }
    if (best < 0) return 0;
    {
        const ShNetGhost* g = ShNet_Ghost(best);
        if (name && cap > 0) ShSocial_FormatName(g->name[0] ? g->name : "a player", g->tag, (g->rf & SHNET_RF_DUPNAME) != 0, name, cap);
        if (outCooldownSec) *outCooldownSec = Inv_CooldownSec(g->playerId, now);
        return g->playerId;
    }
}

/* Who the Invite row asks: anyone online who is not in our game, picked with
 * left and right; the nearest player on our map until one is picked. */
static unsigned int s_pickId;

static int Inv_Candidate(unsigned int id)
{
    return id != 0 && id != ShNet_WorldSelfId() && id != ShNet_SelfId() && !Inv_IsMember(id) && !ShSocial_BlockedId(id);
}

/* Candidates in order: the nearest on our map, then the roster. */
static int Inv_Candidates(unsigned int* ids, char (*names)[SHNET_NAME_MAX + 8], int* maps, int* seats, int cap)
{
    int          n = 0, i, k, cnt;
    char         nm[SHNET_NAME_MAX + 8];
    unsigned int g = ShNetInvite_Nearest(nm, (int)sizeof(nm), NULL);
    if (g != 0 && Inv_Candidate(g) && n < cap)
    {
        ids[n] = g;
        snprintf(names[n], SHNET_NAME_MAX + 8, "%s", nm);
        maps[n]  = (g_SavegamePtr != NULL) ? (int)g_SavegamePtr->mapIdx : -1;
        seats[n] = 1;
        cnt = ShNet_PeerCount();
        for (i = 0; i < cnt; i++)
        {
            const ShNetPeer* p = ShNet_Peer(i);
            if (p && p->playerId == g && (p->rf & SHNET_RF_SPLIT)) seats[n] = 2;
        }
        n++;
    }
    cnt = ShNet_PeerCount();
    for (i = 0; i < cnt && n < cap; i++)
    {
        const ShNetPeer* p = ShNet_Peer(i);
        if (!p || !Inv_Candidate(p->playerId)) continue;
        for (k = 0; k < n && ids[k] != p->playerId; k++) {}
        if (k < n) continue;
        ids[n] = p->playerId;
        ShSocial_FormatName(p->name[0] ? p->name : "a player", p->tag, (p->rf & SHNET_RF_DUPNAME) != 0, names[n],
                            SHNET_NAME_MAX + 8);
        maps[n]  = p->mapIdx;
        seats[n] = (p->rf & SHNET_RF_SPLIT) ? 2 : 1;
        n++;
    }
    return n;
}

unsigned int ShNetInvite_Target(char* name, int cap, int* outMap, int* outCooldownSec)
{
    unsigned int ids[32];
    char         names[32][SHNET_NAME_MAX + 8];
    int          maps[32], seats[32], n, i = 0;

    if (name && cap > 0) name[0] = '\0';
    if (outMap) *outMap = -1;
    if (outCooldownSec) *outCooldownSec = 0;
    if (!Inv_InGame() && !Inv_AtMenus()) return 0;
    n = Inv_Candidates(ids, names, maps, seats, 32);
    if (n == 0) return 0;
    while (i < n && ids[i] != s_pickId) i++;
    if (i == n) i = 0; /* nothing picked, or they left: the first one */
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", names[i]);
    if (outMap) *outMap = maps[i];
    if (outCooldownSec) *outCooldownSec = Inv_CooldownSec(ids[i], SDL_GetTicks());
    return ids[i];
}

void ShNetInvite_Cycle(int dir)
{
    unsigned int ids[32];
    char         names[32][SHNET_NAME_MAX + 8];
    int          maps[32], seats[32], n, i = 0;
    n = Inv_Candidates(ids, names, maps, seats, 32);
    if (n == 0) return;
    while (i < n && ids[i] != s_pickId) i++;
    if (i == n) i = 0;
    i = (i + n + dir) % n;
    s_pickId = ids[i];
}

/* The newest invite still waiting for an answer. */
static int Inv_LastOut(void)
{
    int i, best = -1;
    for (i = 0; i < INV_OUT_MAX; i++)
    {
        if (s_out[i].to && (best < 0 || (int)(s_out[i].ms - s_out[best].ms) > 0)) best = i;
    }
    return best;
}

int ShNetInvite_Outgoing(char* name, int cap)
{
    const unsigned int now = SDL_GetTicks();
    int                left, k = Inv_LastOut();
    if (name && cap > 0) name[0] = '\0';
    if (k < 0) return 0;
    left = (int)((s_out[k].saving ? INV_SAVE_WAIT_MS : INV_EXPIRE_MS) - (now - s_out[k].ms));
    if (left < 1000) left = 1000;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", s_out[k].name);
    return left / 1000;
}

int ShNetInvite_OutgoingSaving(void)
{
    int k = Inv_LastOut();
    return k >= 0 && s_out[k].saving;
}

int ShNetInvite_Incoming(char* name, int cap)
{
    const unsigned int now = SDL_GetTicks();
    int left;
    if (name && cap > 0) name[0] = '\0';
    if (s_inFrom == 0) return 0;
    left = (int)(INV_EXPIRE_MS - (now - s_inMs));
    if (left < 1000) left = 1000;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", s_inName);
    return left / 1000;
}

static int Inv_OutSlot(unsigned int id)
{
    int i;
    for (i = 0; i < INV_OUT_MAX; i++)
        if (s_out[i].to == id && id) return i;
    return -1;
}

/* The invite goes out now: we host a room on the server it can take them into. */
static void Inv_SendNow(unsigned int id, const char* name, unsigned int now)
{
    int i, slot = Inv_OutSlot(id);
    for (i = 0; slot < 0 && i < INV_OUT_MAX; i++)
        if (!s_out[i].to) slot = i;
    if (slot < 0)
    {
        ShNet_PostLocalEvent("Four invites are already waiting for an answer");
        return;
    }
    if (!ShNet_SendInvite(id, SHNET_INV_INVITE, 0)) return;
    s_out[slot].to     = id;
    s_out[slot].ms     = now;
    s_out[slot].saving = 0;
    snprintf(s_out[slot].name, sizeof(s_out[slot].name), "%s", name);
    SH_DBG("[INVITE] invited %s (%u)", name, id);
    Inv_Toast("Invite sent to %s", name);
}

static void Inv_Want(unsigned int id, const char* name, int seats, unsigned int now)
{
    int i, slot = -1;
    for (i = 0; i < INV_OUT_MAX; i++)
        if (s_want[i].to == id) slot = i;
    for (i = 0; slot < 0 && i < INV_OUT_MAX; i++)
        if (!s_want[i].to) slot = i;
    if (slot < 0) slot = 0;
    s_want[slot].to    = id;
    s_want[slot].seats = seats;
    s_want[slot].ms    = now;
    snprintf(s_want[slot].name, sizeof(s_want[slot].name), "%s", name);
}

const char* ShNetInvite_WaitingName(void)
{
    int i;
    for (i = 0; i < INV_OUT_MAX; i++)
        if (s_want[i].to) return s_want[i].name;
    return "";
}

void ShNetInvite_DropWaiting(void)
{
    memset(s_want, 0, sizeof(s_want));
    s_hostFromHere = 0;
}

int ShNetInvite_SendTo(unsigned int id, const char* name, int seats)
{
    const unsigned int now = SDL_GetTicks();
    int                cool;
    if (!ShNet_WorldConnected())
    {
        ShNet_PostLocalEvent("Not connected to the server");
        return 0;
    }
    if (Inv_IsMember(id))
    {
        Inv_Toast("%s is already in your game", name);
        return 0;
    }
    if ((cool = Inv_CooldownSec(id, now)) > 0)
    {
        Inv_Toast("%s declined a moment ago; wait before asking again", name);
        return 0;
    }
    if (ShSession_Active())
    {
        if (!ShSession_IsHost())
        {
            ShNet_PostLocalEvent("Only the host can invite players to this game");
            return 0;
        }
        if (!Inv_ServerRoom())
        {
            ShNet_PostLocalEvent("Invites need a game hosted on the server");
            return 0;
        }
        if (seats == 2 && Inv_MaxPlayers() < 3)
        {
            Inv_Toast("%s plays split screen: your game needs room for 3 players", name);
            return 0;
        }
        if (ShSession_MemberCount() + seats > Inv_MaxPlayers())
        {
            ShNet_PostLocalEvent("Your game is full");
            return 0;
        }
        Inv_SendNow(id, name, now);
        return 1;
    }
    if (Inv_InGame())
    {
        if (ShNetInvite_Blocked())
        {
            ShNet_PostLocalEvent("Can't invite during a boss fight");
            return 0;
        }
        /* Not in a multiplayer game yet: host one from here, or from the lobby. */
        Inv_Want(id, name, seats, now);
        Pc_CoopMenu_OpenInviteHostChoice();
        return 1;
    }
    if (Inv_AtMenus())
    {
        Inv_Want(id, name, seats, now);
        Pc_CoopMenu_OpenHostForInvite();
        Inv_Toast("Host a game: the invite to %s goes out when it opens", name);
        return 1;
    }
    ShNet_PostLocalEvent("Invites can be sent from the game or the menus");
    return 0;
}

void ShNetInvite_Send(void)
{
    unsigned int ids[32];
    char         names[32][SHNET_NAME_MAX + 8];
    int          maps[32], seats[32], n, i = 0;
    n = Inv_Candidates(ids, names, maps, seats, 32);
    if (n == 0) return;
    while (i < n && ids[i] != s_pickId) i++;
    if (i == n) i = 0;
    ShNetInvite_SendTo(ids[i], names[i], seats[i]);
}

void ShNetInvite_HostFromHere(void)
{
    if (!Pc_CoopMenu_InviteHost())
    {
        ShNet_PostLocalEvent("Could not open a room on the server");
        ShNetInvite_DropWaiting();
        return;
    }
    s_hostFromHere   = 1;
    s_hostFromHereMs = SDL_GetTicks();
    ShNet_PostLocalEvent("Opening your game to other players...");
}

void ShNetInvite_Cancel(void)
{
    int k = Inv_LastOut();
    if (k < 0) return;
    ShNet_SendInvite(s_out[k].to, SHNET_INV_CANCEL, 0);
    SH_DBG("[INVITE] cancelled the invite to %s%s", s_out[k].name, s_out[k].saving ? " while they were saving" : "");
    Inv_Toast("Invite to %s cancelled", s_out[k].name);
    /* Pulling out on someone who already said yes costs the same wait as a
     * decline would, so a cancel cannot be used to hurry them. */
    if (s_out[k].saving) Inv_CooldownAdd(s_out[k].to, SDL_GetTicks());
    s_out[k].to = 0;
}

void ShNetInvite_Accept(void)
{
    if (s_inFrom == 0) return;
    if (Inv_InGame() && !Inv_CanSaveHere())
    {
        ShNet_PostLocalEvent("Can't accept during a boss fight");
        return;
    }
    s_acceptFrom = s_inFrom;
    s_acceptMs   = SDL_GetTicks();
    snprintf(s_acceptName, sizeof(s_acceptName), "%s", s_inName);
    ShNet_SendInviteEx(s_inFrom, SHNET_INV_ACCEPT, 0, Inv_SeatsNeeded());
    SH_DBG("[INVITE] accepted %s's invite (%d seat%s)", s_inName, Inv_SeatsNeeded(), Inv_SeatsNeeded() == 1 ? "" : "s");
    Inv_Toast("Accepted. Joining %s's game...", s_inName);
    s_inFrom = 0;
}

int ShNetInvite_AcceptAfterSave(void)
{
    if (s_inFrom == 0) return 0;
    if (!Inv_CanSaveHere())
    {
        ShNet_PostLocalEvent("Can't accept during a boss fight");
        return 0;
    }
    if (!Pc_Save_OpenSaveScreen()) return 0;
    s_saveFrom      = s_inFrom;
    s_saveMs        = SDL_GetTicks();
    s_savePhase     = 1;
    s_saveCancelled = 0;
    snprintf(s_saveName, sizeof(s_saveName), "%s", s_inName);
    s_inFrom = 0;
    ShNet_SendInviteEx(s_saveFrom, SHNET_INV_SAVING, 0, Inv_SeatsNeeded());
    SH_DBG("[INVITE] saving before accepting %s's invite", s_saveName);
    return 1;
}

/* Back from the save screen (saved or not): the accept we put off. */
static void Inv_SaveDone(unsigned int now)
{
    const unsigned int from = s_saveFrom;
    s_savePhase = 0;
    s_saveFrom  = 0;
    if (s_saveCancelled)
    {
        Inv_Toast("%s withdrew the invite", s_saveName);
        return;
    }
    s_acceptFrom = from;
    s_acceptMs   = now;
    snprintf(s_acceptName, sizeof(s_acceptName), "%s", s_saveName);
    ShNet_SendInviteEx(from, SHNET_INV_ACCEPT, 0, Inv_SeatsNeeded());
    SH_DBG("[INVITE] saved; accepted %s's invite", s_saveName);
    Inv_Toast("Accepted. Joining %s's game...", s_saveName);
}

void ShNetInvite_Decline(void)
{
    if (s_inFrom == 0) return;
    ShNet_SendInvite(s_inFrom, SHNET_INV_DECLINE, 0);
    SH_DBG("[INVITE] declined %s's invite", s_inName);
    s_inFrom = 0;
}

/* ------------------------------------------------------------------ */
/* Joining a room by code                                              */
/* ------------------------------------------------------------------ */

/* Player 1 of a split-screen pair takes Player 2 along: the code goes to
 * their window, which follows on its own server link. */
static void Inv_TellPlayer2(int code, int lobby)
{
    unsigned char b[4];
    if (Pc_Split_Role() != 1 || !ShSession_Active()) return;
    b[0] = SHNET_CTRL_FOLLOW;
    b[1] = (unsigned char)(code & 0xFF);
    b[2] = (unsigned char)((code >> 8) & 0xFF);
    b[3] = (unsigned char)(lobby ? 1 : 0);
    ShSession_SendGame(SHNET_MSG_S_CTRL, b, 4, 0, 1);
    SH_DBG("[INVITE] told Player 2 to follow into room %d", code);
}

void ShNetInvite_JoinCode(int code, const char* name)
{
    if (!g_PcConfig.onlineServer[0])
    {
        ShNet_PostLocalEvent("No server is set (Multiplayer Settings)");
        return;
    }
    if (Inv_InGame() && !Inv_CanSaveHere() && !g_PcCoopGame)
    {
        ShNet_PostLocalEvent("Can't join a game during a boss fight");
        return;
    }
    if (ShSession_Active() && ShSession_LobbyId() == (unsigned long long)code)
    {
        ShNet_PostLocalEvent("You are already in that game");
        return;
    }
    if (ShSession_Active() && ShSession_IsHost() && Inv_Others() > 0)
    {
        ShNet_PostLocalEvent("Close the game you are hosting first");
        return;
    }
    s_joinCode = code;
    s_joinMs   = SDL_GetTicks();
    snprintf(s_joinName, sizeof(s_joinName), "%s", name && name[0] ? name : "the host");
    if (ShSession_Active() && Pc_Split_Role() == 1)
    {
        /* Player 2 hears it over the room we are about to leave. */
        Inv_TellPlayer2(code, s_joinLobby);
        s_joinStep = 4;
    }
    else if (ShSession_Active())
    {
        ShSession_RequestLeave();
        s_joinStep = 1;
    }
    else
    {
        s_joinStep = 2;
    }
    SH_DBG("[INVITE] joining room %d%s", code, s_joinLobby ? " (host in the lobby)" : "");
}

void ShNetInvite_OnFollow(int code, int lobby)
{
    if (Pc_Split_Role() != 2) return;
    s_joinLobby = lobby;
    ShNetInvite_JoinCode(code, "Player 1's host");
}

static void Inv_JoinTick(unsigned int now)
{
    char why[128];
    switch (s_joinStep)
    {
    case 4:
        if (now - s_joinMs > INV_FOLLOW_MS)
        {
            ShSession_RequestLeave();
            s_joinMs   = now;
            s_joinStep = 1;
        }
        break;

    case 1:
        if (!ShSession_Active() || now - s_joinMs > INV_LEAVE_MS)
        {
            s_joinStep = 2;
        }
        break;

    case 2:
        s_joinMs = now;
        if (Inv_InGame() && s_joinLobby)
        {
            /* The host is in the lobby: our game ends here and we wait there
             * with them, on the title, until they start. */
            Pc_CoopMenu_JoinAtTitle(s_joinCode);
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            s_joinStep = 0;
            s_joinLobby = 0;
            break;
        }
        if (Inv_InGame())
        {
            Pc_CoopMenu_InviteJoin(s_joinCode);
            s_joinStep = 3;
        }
        else
        {
            Pc_CoopMenu_JoinAtTitle(s_joinCode);
            s_joinStep = 0;
        }
        s_joinLobby = 0;
        break;

    case 3:
        if (ShSession_Active() && !ShSession_IsHost())
        {
            int map = ShSession_TakeWorldRequest();
            if (map >= 0)
            {
                s_Savegame save;
                if (ShSession_TakeWorldSave(&save, (int)sizeof(save)) && save.mapIdx == map)
                {
                    SH_DBG("[INVITE] in %s's room; booting their save (map %d)", s_joinName, map);
                    g_PcCoopGame = 1;
                    Pc_CoopMenu_RequestReboot(&save);
                    s_joinStep = 0;
                }
                else
                {
                    SH_DBG("[INVITE] world request for map %d came without a save; waiting", map);
                }
            }
            else if (now - s_joinMs > INV_LOBBY_MS && ShSession_MemberCount() > 1)
            {
                /* In, and no world after a while: they are still in the lobby. */
                SH_DBG("[INVITE] %s is in the lobby; waiting on the title with them", s_joinName);
                Pc_CoopMenu_LobbyAtTitle();
                g_SysWork.sysFlags |= SysFlag_DoWarmReset;
                s_joinStep = 0;
            }
        }
        else if (now - s_joinMs > 3000u && !ShSession_Active())
        {
            ShSession_StatusLine(why, (int)sizeof(why));
            if (strstr(why, "full") || strstr(why, "Unable") || strstr(why, "removed") || strstr(why, "build") ||
                strstr(why, "no such"))
            {
                char line[160];
                snprintf(line, sizeof(line), "Could not join: %s", why);
                ShNet_PostLocalEvent(line);
                s_joinStep = 0;
            }
        }
        if (s_joinStep == 3 && now - s_joinMs > INV_JOIN_WAIT_MS)
        {
            Inv_Toast("Could not join %s's game", s_joinName);
            if (ShSession_Active() && !g_PcCoopGame) ShSession_RequestLeave();
            s_joinStep = 0;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The conversation                                                    */
/* ------------------------------------------------------------------ */

/* Someone asking us now: may we take it? A guest in someone else's game may
 * (they leave it); a host with others in their game may not. */
static int Inv_Busy(const char** why)
{
    if (s_inFrom || s_joinStep || s_acceptFrom || s_saveFrom) { *why = "weighing another invite"; return 1; }
    if (ShSession_Active() && ShSession_IsHost() && Inv_Others() > 0) { *why = "hosting a game"; return 1; }
    if (Pc_Split_Role() == 2) { *why = "split-screen Player 2"; return 1; }
    if (Inv_AtMenus()) return 0;
    if (!Inv_CanSaveHere()) { *why = "not in plain gameplay"; return 1; }
    return 0;
}

static void Inv_Handle(unsigned int from, int action, int code, int extra, const char* name, unsigned int now)
{
    int k = Inv_OutSlot(from);
    switch (action)
    {
    case SHNET_INV_INVITE:
    {
        const char* why = "";
        /* Someone we blocked: no answer at all, as if we never heard. */
        if (ShSocial_BlockedId(from)) break;
        if (s_inFrom == from)
        {
            s_inMs = now; /* asked again: the same invite, fresh */
            break;
        }
        if (Inv_Busy(&why))
        {
            SH_DBG("[INVITE] %s invited us; busy (%s)", name[0] ? name : "a player", why);
            ShNet_SendInvite(from, SHNET_INV_BUSY, 0);
            break;
        }
        s_inFrom = from;
        s_inMs   = now;
        snprintf(s_inName, sizeof(s_inName), "%s", name[0] ? name : "A player");
        SH_DBG("[INVITE] %s (%u) invites us", s_inName, from);
        Inv_Toast(Inv_AtMenus() ? "%s invites you to their game. Type /accept or /decline in chat"
                                : "%s invites you to their game. Press M (or type /accept)", s_inName);
        break;
    }

    case SHNET_INV_CANCEL:
        if (s_inFrom == from)
        {
            s_inFrom = 0;
            Inv_Toast("%s withdrew the invite", name[0] ? name : "They");
        }
        else if (s_acceptFrom == from)
        {
            s_acceptFrom = 0;
            Inv_Toast("%s's invite is no longer open", name[0] ? name : "Their");
        }
        else if (s_saveFrom == from)
        {
            s_saveCancelled = 1; /* told on the way back; the save goes on */
        }
        break;

    case SHNET_INV_SAVING:
        if (k >= 0 && !s_out[k].saving)
        {
            s_out[k].saving = 1;
            s_out[k].ms     = now;
            SH_DBG("[INVITE] %s is saving before joining", s_out[k].name);
            Inv_Toast("%s is saving their game first...", s_out[k].name);
        }
        break;

    case SHNET_INV_ACCEPT:
        if (k < 0 || !Inv_HostingServerRoom())
        {
            ShNet_SendInvite(from, SHNET_INV_CANCEL, 0); /* too late, or our game is gone */
            if (k >= 0) s_out[k].to = 0;
            break;
        }
        /* In they come: to the game where it is, or to the lobby. */
        ShNet_SendInviteEx(from, SHNET_INV_JOIN, (int)ShSession_LobbyId(),
                           (g_PcCoopGame && Inv_InGame()) ? 0 : SHNET_INVJ_LOBBY);
        SH_DBG("[INVITE] %s accepted; sent our room code", s_out[k].name);
        Inv_Toast("%s is joining your game", s_out[k].name);
        s_out[k].to = 0;
        break;

    case SHNET_INV_DECLINE:
        if (k >= 0)
        {
            Inv_CooldownAdd(from, now);
            Inv_Toast("%s declined", s_out[k].name);
            s_out[k].to = 0;
        }
        break;

    case SHNET_INV_BUSY:
        if (k >= 0)
        {
            SH_DBG("[INVITE] %s is busy", s_out[k].name);
            Inv_Toast("%s is busy right now (hosting a game, a menu or a scene)", s_out[k].name);
            s_out[k].to = 0;
        }
        break;

    case SHNET_INV_GONE:
        if (k >= 0)
        {
            Inv_Toast("%s is no longer here", s_out[k].name);
            s_out[k].to = 0;
        }
        break;

    case SHNET_INV_FULL:
        /* The server, to both ends: no seat left for this yes. */
        if (k >= 0)
        {
            Inv_Toast("%s could not join: your game is full", s_out[k].name);
            s_out[k].to = 0;
        }
        else if (s_acceptFrom == from || s_saveFrom == from)
        {
            Inv_Toast("%s's game is full", name[0] ? name : "That");
            s_acceptFrom = 0;
            if (s_saveFrom == from) s_saveCancelled = 1;
        }
        break;

    case SHNET_INV_JOIN:
        if (s_acceptFrom != from)
        {
            break;
        }
        s_acceptFrom = 0;
        s_joinLobby  = (extra & SHNET_INVJ_LOBBY) != 0;
        SH_DBG("[INVITE] joining %s's room %d%s", s_acceptName, code, s_joinLobby ? " (lobby)" : "");
        ShNetInvite_JoinCode(code, s_acceptName);
        break;

    default:
        break;
    }
}

void ShNetInvite_Tick(void)
{
    const unsigned int now = SDL_GetTicks();
    unsigned int       from;
    int                action, code, extra = 0, i;
    char               name[SHNET_NAME_MAX];

    while (ShNet_TakeInviteEx(&from, &action, &code, name, (int)sizeof(name), &extra))
    {
        Inv_Handle(from, action, code, extra, name, now);
    }

    for (i = 0; i < INV_OUT_MAX; i++)
    {
        if (s_out[i].to && now - s_out[i].ms > (s_out[i].saving ? INV_SAVE_WAIT_MS : INV_EXPIRE_MS))
        {
            ShNet_SendInvite(s_out[i].to, SHNET_INV_CANCEL, 0);
            Inv_Toast("No answer from %s", s_out[i].name);
            if (s_out[i].saving) Inv_CooldownAdd(s_out[i].to, now);
            s_out[i].to = 0;
        }
    }

    /* Invites waiting for our room: out they go once it is open. */
    if (Inv_HostingServerRoom())
    {
        if (s_hostFromHere && !g_PcCoopGame && Inv_InGame())
        {
            /* This game becomes the co-op host game: the proven way in is a
             * reboot into our own save where we stand. */
            s_hostFromHere = 0;
            Pc_CoopMenu_RequestHostReboot();
        }
        for (i = 0; i < INV_OUT_MAX; i++)
        {
            if (!s_want[i].to) continue;
            if (s_want[i].seats == 2 && Inv_MaxPlayers() < 3)
                Inv_Toast("%s plays split screen: your game needs room for 3 players", s_want[i].name);
            else
                Inv_SendNow(s_want[i].to, s_want[i].name, now);
            s_want[i].to = 0;
        }
    }
    else
    {
        if (s_hostFromHere && now - s_hostFromHereMs > INV_HOST_WAIT_MS)
        {
            ShNet_PostLocalEvent("Your room did not open on the server");
            if (ShSession_Active() && !g_PcCoopGame) ShSession_RequestLeave();
            ShNetInvite_DropWaiting();
        }
        for (i = 0; i < INV_OUT_MAX; i++)
        {
            if (s_want[i].to && now - s_want[i].ms > INV_PEND_MS) s_want[i].to = 0;
        }
    }

    if (s_inFrom && now - s_inMs > INV_EXPIRE_MS)
    {
        Inv_Toast("%s's invite expired", s_inName);
        s_inFrom = 0;
    }
    if (s_acceptFrom && now - s_acceptMs > INV_EXPIRE_MS)
    {
        s_acceptFrom = 0;
    }

    if (s_savePhase == 1)
    {
        if (g_GameWork.gameState != GameState_InGame) s_savePhase = 2;
        else if (now - s_saveMs > INV_SAVE_OPEN_MS) Inv_SaveDone(now); /* the screen never came */
    }
    else if (s_savePhase == 2)
    {
        if (g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay) Inv_SaveDone(now);
    }

    Inv_JoinTick(now);
    Pc_CoopMenu_InviteTick();
}
