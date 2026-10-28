/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sh_net_invite.c - inviting a living-world player into your game (see the header). */

#include "sh_net_invite.h"

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "pc_config.h"
#include "pc_coop_menu.h"
#include "pc_coop_save.h" /* Pc_Save_BossActive */
#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_log.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

#define INV_EXPIRE_MS    30000u  /* an invite, on both ends */
#define INV_COOLDOWN_MS  120000u /* after a decline, before asking that player again */
#define INV_HOST_WAIT_MS 15000u  /* the room has this long to open after an accept */
#define INV_JOIN_WAIT_MS 45000u  /* the guest has this long to be in the room and hear S_WORLD */
#define INV_COOL_MAX     8

/* The invite we sent. */
static unsigned int s_outTo;
static char         s_outName[SHNET_NAME_MAX];
static unsigned int s_outMs;
static int          s_outPhase; /* 0 none, 1 asked, 2 accepted and the room is opening */

/* The invite waiting for our answer, and the one we said yes to. */
static unsigned int s_inFrom;
static char         s_inName[SHNET_NAME_MAX];
static unsigned int s_inMs;
static unsigned int s_acceptFrom;
static char         s_acceptName[SHNET_NAME_MAX];
static unsigned int s_acceptMs;

/* Guest: joined the inviter's room, waiting for their world to boot into. */
static int          s_joinPhase;
static unsigned int s_joinMs;

static struct { unsigned int id; unsigned int untilMs; } s_cool[INV_COOL_MAX];

static int Inv_InGame(void)
{
    return g_GameWork.gameState == GameState_InGame && g_SavegamePtr != NULL;
}

/* Where the game could be saved: accepting makes a save of the host's game on
 * the spot and drops the guest into it, so neither side may be mid boss
 * fight, the one place the quick save refuses too. */
static int Inv_CanSaveHere(void)
{
    return Inv_InGame() && g_SysWork.sysState == SysState_Gameplay && !Pc_Save_BossActive();
}

static void Inv_Toast(const char* fmt, const char* name)
{
    char line[96];
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

int ShNetInvite_Blocked(void)
{
    return Inv_InGame() && !Inv_CanSaveHere();
}

int ShNetInvite_CanInvite(void)
{
    if (!ShNet_WorldConnected() || !Inv_InGame()) return 0;
    if (!ShSession_Active()) return 1;
    /* Hosting a server room with a seat free; a Steam lobby cannot take a
     * living-world player, and a guest is not the one who invites. */
    return ShSession_IsHost() && ShSession_LobbyId() != 0 && ShSession_LobbyId() < 100000ull &&
           ShSession_MemberCount() < Inv_MaxPlayers();
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
        if (!ShNet_GhostInterp(i, &x, &y, &z, &r)) { x = g->curX; z = g->curZ; }
        dx = (long long)x - mx;
        dz = (long long)z - mz;
        d  = dx * dx + dz * dz;
        if (best < 0 || d < bestD) { best = i; bestD = d; }
    }
    if (best < 0) return 0;
    {
        const ShNetGhost* g = ShNet_Ghost(best);
        if (name && cap > 0) snprintf(name, (size_t)cap, "%s", g->name[0] ? g->name : "a player");
        if (outCooldownSec) *outCooldownSec = Inv_CooldownSec(g->playerId, now);
        return g->playerId;
    }
}

int ShNetInvite_Outgoing(char* name, int cap)
{
    const unsigned int now = SDL_GetTicks();
    int left;
    if (name && cap > 0) name[0] = '\0';
    if (s_outPhase == 0) return 0;
    left = (int)(INV_EXPIRE_MS - (now - s_outMs));
    if (left < 1000) left = 1000;
    if (name && cap > 0) snprintf(name, (size_t)cap, "%s", s_outName);
    return left / 1000;
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

static void Inv_ClearOut(void)
{
    s_outPhase = 0;
    s_outTo    = 0;
}

void ShNetInvite_Send(void)
{
    const unsigned int now = SDL_GetTicks();
    char               name[SHNET_NAME_MAX];
    int                cool = 0;
    unsigned int       id;

    if (s_outPhase || !ShNetInvite_CanInvite()) return;
    if (ShNetInvite_Blocked())
    {
        ShNet_PostLocalEvent("Can't invite during a boss fight");
        return;
    }
    id = ShNetInvite_Nearest(name, (int)sizeof(name), &cool);
    if (id == 0) return;
    if (cool > 0)
    {
        Inv_Toast("%s declined a moment ago; wait before asking again", name);
        return;
    }
    if (!ShNet_SendInvite(id, SHNET_INV_INVITE, 0)) return;
    s_outTo    = id;
    s_outMs    = now;
    s_outPhase = 1;
    snprintf(s_outName, sizeof(s_outName), "%s", name);
    SH_DBG("[INVITE] invited %s (%u)", name, id);
    Inv_Toast("Invite sent to %s", name);
}

void ShNetInvite_Cancel(void)
{
    if (s_outPhase == 0) return;
    ShNet_SendInvite(s_outTo, SHNET_INV_CANCEL, 0);
    SH_DBG("[INVITE] cancelled the invite to %s", s_outName);
    Inv_Toast("Invite to %s cancelled", s_outName);
    Inv_ClearOut();
}

void ShNetInvite_Accept(void)
{
    if (s_inFrom == 0) return;
    if (!Inv_CanSaveHere())
    {
        ShNet_PostLocalEvent("Can't accept during a boss fight");
        return;
    }
    s_acceptFrom = s_inFrom;
    s_acceptMs   = SDL_GetTicks();
    snprintf(s_acceptName, sizeof(s_acceptName), "%s", s_inName);
    ShNet_SendInvite(s_inFrom, SHNET_INV_ACCEPT, 0);
    SH_DBG("[INVITE] accepted %s's invite", s_inName);
    Inv_Toast("Accepted. Waiting for %s's game...", s_inName);
    s_inFrom = 0;
}

void ShNetInvite_Decline(void)
{
    if (s_inFrom == 0) return;
    ShNet_SendInvite(s_inFrom, SHNET_INV_DECLINE, 0);
    SH_DBG("[INVITE] declined %s's invite", s_inName);
    s_inFrom = 0;
}

static void Inv_Handle(unsigned int from, int action, int code, const char* name, unsigned int now)
{
    switch (action)
    {
    case SHNET_INV_INVITE:
        if (s_inFrom == from)
        {
            s_inMs = now; /* asked again: the same invite, fresh */
            break;
        }
        if (ShSession_Active() || s_inFrom || s_outPhase || s_joinPhase || s_acceptFrom || !Inv_CanSaveHere())
        {
            ShNet_SendInvite(from, SHNET_INV_BUSY, 0);
            break;
        }
        s_inFrom = from;
        s_inMs   = now;
        snprintf(s_inName, sizeof(s_inName), "%s", name[0] ? name : "A player");
        SH_DBG("[INVITE] %s (%u) invites us", s_inName, from);
        Inv_Toast("%s invites you to their game. Press M to answer", s_inName);
        break;

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
        break;

    case SHNET_INV_ACCEPT:
        if (s_outPhase != 1 || s_outTo != from)
        {
            ShNet_SendInvite(from, SHNET_INV_CANCEL, 0); /* too late */
            break;
        }
        if (!Inv_CanSaveHere())
        {
            /* The fight started after the invite went out. */
            ShNet_SendInvite(from, SHNET_INV_CANCEL, 0);
            Inv_Toast("Can't bring %s in during a boss fight", s_outName);
            Inv_ClearOut();
            break;
        }
        if (ShSession_Active() && ShSession_IsHost() && ShSession_LobbyId() != 0 && ShSession_LobbyId() < 100000ull)
        {
            /* Already hosting a room: in they come. */
            ShNet_SendInvite(from, SHNET_INV_JOIN, (int)ShSession_LobbyId());
            SH_DBG("[INVITE] %s accepted; sent the code of the room we host", s_outName);
            Inv_Toast("%s is joining your game", s_outName);
            Inv_ClearOut();
            break;
        }
        if (!Pc_CoopMenu_InviteHost())
        {
            ShNet_SendInvite(from, SHNET_INV_CANCEL, 0);
            Inv_Toast("Could not open a room for %s", s_outName);
            Inv_ClearOut();
            break;
        }
        s_outPhase = 2;
        s_outMs    = now;
        SH_DBG("[INVITE] %s accepted; opening a room", s_outName);
        Inv_Toast("%s accepted. Opening a room...", s_outName);
        break;

    case SHNET_INV_DECLINE:
        if (s_outPhase && s_outTo == from)
        {
            Inv_CooldownAdd(from, now);
            Inv_Toast("%s declined", s_outName);
            Inv_ClearOut();
        }
        break;

    case SHNET_INV_BUSY:
        if (s_outPhase && s_outTo == from)
        {
            Inv_Toast("%s is busy right now", s_outName);
            Inv_ClearOut();
        }
        break;

    case SHNET_INV_GONE:
        if (s_outPhase && s_outTo == from)
        {
            Inv_Toast("%s is no longer here", s_outName);
            Inv_ClearOut();
        }
        break;

    case SHNET_INV_JOIN:
        if (s_acceptFrom != from || ShSession_Active() || !Inv_InGame())
        {
            break;
        }
        s_acceptFrom = 0;
        s_joinPhase  = 1;
        s_joinMs     = now;
        Pc_CoopMenu_InviteJoin(code);
        SH_DBG("[INVITE] joining %s's room %d", s_acceptName, code);
        Inv_Toast("Joining %s's game...", s_acceptName);
        break;

    default:
        break;
    }
}

void ShNetInvite_Tick(void)
{
    const unsigned int now = SDL_GetTicks();
    unsigned int       from;
    int                action, code;
    char               name[SHNET_NAME_MAX];

    while (ShNet_TakeInvite(&from, &action, &code, name, (int)sizeof(name)))
    {
        Inv_Handle(from, action, code, name, now);
    }

    if (s_outPhase == 1 && now - s_outMs > INV_EXPIRE_MS)
    {
        ShNet_SendInvite(s_outTo, SHNET_INV_CANCEL, 0);
        Inv_Toast("No answer from %s", s_outName);
        Inv_ClearOut();
    }
    else if (s_outPhase == 2)
    {
        if (!Inv_CanSaveHere())
        {
            ShNet_SendInvite(s_outTo, SHNET_INV_CANCEL, 0);
            Inv_Toast("Can't bring %s in during a boss fight", s_outName);
            Inv_ClearOut();
            if (ShSession_Active() && !g_PcCoopGame) ShSession_RequestLeave();
        }
        else if (ShSession_Active() && ShSession_IsHost() && ShSession_LobbyId() != 0 && ShSession_LobbyId() < 100000ull)
        {
            ShNet_SendInvite(s_outTo, SHNET_INV_JOIN, (int)ShSession_LobbyId());
            SH_DBG("[INVITE] room %llu open; code sent to %s", ShSession_LobbyId(), s_outName);
            Inv_Toast("%s is joining your game", s_outName);
            Inv_ClearOut();
            /* This game becomes the co-op host game: the proven way in is a
             * reboot into our own save where we stand. */
            if (!g_PcCoopGame)
            {
                Pc_CoopMenu_RequestHostReboot();
            }
        }
        else if (now - s_outMs > INV_HOST_WAIT_MS)
        {
            ShNet_SendInvite(s_outTo, SHNET_INV_CANCEL, 0);
            Inv_Toast("The room for %s did not open", s_outName);
            Inv_ClearOut();
            if (ShSession_Active() && !g_PcCoopGame) ShSession_RequestLeave();
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

    if (s_joinPhase)
    {
        if (ShSession_Active() && !ShSession_IsHost())
        {
            int map = ShSession_TakeWorldRequest();
            if (map >= 0)
            {
                s_Savegame save;
                if (ShSession_TakeWorldSave(&save, (int)sizeof(save)) && save.mapIdx == map)
                {
                    SH_DBG("[INVITE] in %s's room; booting their save (map %d)", s_acceptName, map);
                    g_PcCoopGame = 1;
                    Pc_CoopMenu_RequestReboot(&save);
                    s_joinPhase = 0;
                }
                else
                {
                    SH_DBG("[INVITE] world request for map %d came without a save; waiting", map);
                }
            }
        }
        if (s_joinPhase && now - s_joinMs > INV_JOIN_WAIT_MS)
        {
            Inv_Toast("Could not join %s's game", s_acceptName);
            if (ShSession_Active() && !g_PcCoopGame) ShSession_RequestLeave();
            s_joinPhase = 0;
        }
    }
}
