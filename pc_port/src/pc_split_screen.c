/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_split_screen.c - two players on one PC. See pc_split_screen.h.
 *
 * Player 1's process is the host: it opens a room on the embedded relay (the
 * "Host on: This PC" path), launches a second copy of this exe for Player 2
 * with everything that copy needs on its command line, tiles the two windows
 * over the display, and stops reading Player 2's controller. Player 2's copy
 * joins the room over loopback as soon as it reaches the title screen, and
 * the host's lobby starts the game by itself the moment the second member is
 * in. From then on it is an ordinary two-member co-op game.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/sound/sound_system.h" /* Sd_SetVolume: Player 2's window plays effects only */

#include "pc_split_screen.h"
#include "pc_config.h"
#include "pc_coop_menu.h"
#include "pc_playas.h"
#include "sh_net_session.h"
#include "sh_log.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pc_split_proc.c (plain types: that file holds <windows.h>) */
extern void*         PcProc_Spawn(const char* cmdline);
extern int           PcProc_Alive(void* h);
extern void          PcProc_Kill(void* h);
extern unsigned long PcProc_SelfPid(void);
extern int           PcProc_PidAlive(unsigned long pid);
extern int           PcProc_ExePath(char* out, int cap);

/* PsyCross */
extern SDL_Window* g_window;
extern void        PsyX_Pad_SetDeviceFilter(int mode, int deviceId);
extern void        PsyX_ApplyWindowState(int width, int height, int fullscreen);
extern int         g_PsyX_NoKeyboardMouse;

/* pc_config.c: Player 2's copy must never write the shared config.cfg. */
extern int g_PcConfigReadOnly;

#define SPLIT_DEFAULT_PORT 27888

static int s_role; /* 0 none, 1 Player 1's window, 2 Player 2's */

/* ---- What Player 1 put on Player 2's command line ---- */
static int           s_argPad = -1; /* SDL device index of Player 2's controller */
static char          s_argGuid[64]; /* its GUID, in case the indices differ between the two processes */
static int           s_argWin[4];   /* x, y, w, h of Player 2's window */
static int           s_haveWin;
static int           s_argPort;
static char          s_argServer[64]; /* the room is on this server (My Server), not on Player 1's PC */
static int           s_argRoom;       /* its code there */
static unsigned long s_argParent;   /* Player 1's process id: when it is gone, so are we */
static char          s_argName[24];
static int           s_argPlayAs = -1;
static int           s_argAudio;    /* 0 effects only, 1 everything, 2 muted */
static char          s_dataArg[512];

/* ---- Player 1 ---- */
static void*        s_child;         /* Player 2's process */
static int          s_childStopping; /* asked to go; killed if still there after a grace period */
static unsigned int s_childStopMs;
static int          s_hostWaiting;
static unsigned int s_hostWaitSince;
static int          s_sessionSeen;   /* the room came up after Start (so its going away means stop) */
static int          s_windowArranged;
static int          s_atExitSet;
static char         s_status[96];

/* ---- Player 2 ---- */
static int          s_menuOpened;
static unsigned int s_nextJoinMs;
static int          s_joinTries;
static int          s_wasActive;
static unsigned int s_nextParentCheckMs;

int Pc_Split_Role(void)
{
    return s_role;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

int Pc_Split_ParseArg(int argc, char** argv, int* i)
{
    const char* a = argv[*i];
    const char* v = (*i + 1 < argc) ? argv[*i + 1] : NULL;

    if (strcmp(a, "-split2") == 0)
    {
        s_role = 2;
        return 1;
    }
    if (!v)
        return 0;

    if (strcmp(a, "-pad") == 0)          s_argPad = atoi(v);
    else if (strcmp(a, "-padguid") == 0) snprintf(s_argGuid, sizeof(s_argGuid), "%s", v);
    else if (strcmp(a, "-window") == 0)
        s_haveWin = (sscanf(v, "%d,%d,%d,%d", &s_argWin[0], &s_argWin[1], &s_argWin[2], &s_argWin[3]) == 4);
    else if (strcmp(a, "-port") == 0)    s_argPort = atoi(v);
    else if (strcmp(a, "-server") == 0)  snprintf(s_argServer, sizeof(s_argServer), "%s", v);
    else if (strcmp(a, "-room") == 0)    s_argRoom = atoi(v);
    else if (strcmp(a, "-parent") == 0)  s_argParent = strtoul(v, NULL, 10);
    else if (strcmp(a, "-name") == 0)    snprintf(s_argName, sizeof(s_argName), "%s", v);
    else if (strcmp(a, "-playas") == 0)  s_argPlayAs = atoi(v);
    else if (strcmp(a, "-p2audio") == 0) s_argAudio = atoi(v);
    else
        return 0;

    (*i)++;
    return 1;
}

void Pc_Split_NoteDataArg(const char* path)
{
    if (path)
        snprintf(s_dataArg, sizeof(s_dataArg), "%s", path);
}

void Pc_Split_AfterConfigLoad(void)
{
    if (s_role != 2)
        return;

    /* Player 2's window: the half Player 1 gave us, straight to the title (the
     * host starts the game), and nothing that is per-PC rather than per-player:
     * the living world (one ghost per PC), Steam (one client per PC), Discord
     * and RetroAchievements. The config file stays Player 1's. */
    g_PcConfig.fullscreen = 0;
    if (s_haveWin && s_argWin[2] > 0 && s_argWin[3] > 0)
    {
        g_PcConfig.windowWidth  = s_argWin[2];
        g_PcConfig.windowHeight = s_argWin[3];
    }
    g_PcConfig.skipIntros          = 1;
    g_PcConfig.attractDemos        = 0;
    g_PcConfig.onlineEnabled       = 0;
    g_PcConfig.onlineSteam         = 0;
    g_PcConfig.onlineSteamAutoHost = 0;
    g_PcConfig.discordRichPresence = 0;
    g_PcConfig.retroAchievements   = 0;
    g_PcConfig.confineCursor       = 0;
    g_PcConfig.mouseCursor         = 0;
    if (s_argName[0])
        snprintf(g_PcConfig.onlineName, sizeof(g_PcConfig.onlineName), "%s", s_argName);
    if (s_argAudio != 1)
    {
        /* Effects only and muted alike: scenes and movies play on both
         * halves at once, so their voices and music come from Player 1's. */
        g_PcConfig.xaVolume  = 0.0f;
        g_PcConfig.fmvVolume = 0.0f;
    }
    g_PcConfigReadOnly     = 1;
    g_PsyX_NoKeyboardMouse = 1;
}

/* The device index Player 1 named, unless the GUID says the indices differ
 * between the two processes (a pad plugged in between the two enumerations),
 * in which case the first pad with that GUID. */
static int Split_ResolveP2Device(void)
{
    int  n = SDL_NumJoysticks();
    int  j;
    char g[64];

    if (s_argPad >= 0 && s_argPad < n && SDL_IsGameController(s_argPad))
    {
        if (!s_argGuid[0])
            return s_argPad;
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(s_argPad), g, sizeof(g));
        if (strcmp(g, s_argGuid) == 0)
            return s_argPad;
    }
    for (j = 0; j < n; j++)
    {
        if (!SDL_IsGameController(j))
            continue;
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(j), g, sizeof(g));
        if (s_argGuid[0] && strcmp(g, s_argGuid) == 0)
            return j;
    }
    return s_argPad;
}

void Pc_Split_AfterWindow(void)
{
    int dev;

    if (s_role != 2 || !g_window)
        return;

    SDL_SetWindowTitle(g_window, "Silent Hill - Player 2");
    if (s_haveWin)
    {
        SDL_SetWindowBordered(g_window, SDL_FALSE);
        SDL_SetWindowSize(g_window, s_argWin[2], s_argWin[3]);
        SDL_SetWindowPosition(g_window, s_argWin[0], s_argWin[1]);
    }

    dev = Split_ResolveP2Device();
    PsyX_Pad_SetDeviceFilter(1, dev);
    SH_DBG("[SPLIT] Player 2 window: pad device %d (%s), window %d,%d %dx%d, port %d, parent %lu",
           dev, (dev >= 0 && dev < SDL_NumJoysticks() && SDL_GameControllerNameForIndex(dev))
                    ? SDL_GameControllerNameForIndex(dev) : "none",
           s_argWin[0], s_argWin[1], s_argWin[2], s_argWin[3], s_argPort, s_argParent);
}

void Pc_Split_AfterPlayAsInit(void)
{
    if (s_role == 2 && s_argPlayAs >= 0)
        Pc_PlayAs_SetByIndex(s_argPlayAs, 0);
}

/* ------------------------------------------------------------------ */
/* Controllers                                                         */
/* ------------------------------------------------------------------ */

/* The k-th attached game controller's SDL device index, -1 when there is none. */
static int Split_PadDevice(int k)
{
    int n = SDL_NumJoysticks();
    int j, c = 0;

    for (j = 0; j < n; j++)
    {
        if (!SDL_IsGameController(j))
            continue;
        if (c == k)
            return j;
        c++;
    }
    return -1;
}

int Pc_Split_PadCount(void)
{
    int n = SDL_NumJoysticks();
    int j, c = 0;

    for (j = 0; j < n; j++)
        if (SDL_IsGameController(j))
            c++;
    return c;
}

const char* Pc_Split_PadName(int k)
{
    int         d  = Split_PadDevice(k);
    const char* nm = (d >= 0) ? SDL_GameControllerNameForIndex(d) : NULL;
    return nm ? nm : "none";
}

/* ------------------------------------------------------------------ */
/* Player 1                                                            */
/* ------------------------------------------------------------------ */

static void Split_AtExit(void)
{
    if (s_child)
    {
        PcProc_Kill(s_child);
        s_child = NULL;
    }
}

/* Windowed and borderless at the given rect, so the two halves tile the
 * display. PsyX_ApplyWindowState handles the drop out of fullscreen and the
 * present size; the border and position are ours. */
static void Split_ArrangeOwnWindow(const SDL_Rect* r)
{
    if (!g_window)
        return;
    PsyX_ApplyWindowState(r->w, r->h, 0);
    SDL_SetWindowBordered(g_window, SDL_FALSE);
    SDL_SetWindowSize(g_window, r->w, r->h);
    SDL_SetWindowPosition(g_window, r->x, r->y);
    s_windowArranged = 1;
}

static void Split_RestoreOwnWindow(void)
{
    if (!g_window || !s_windowArranged)
        return;
    SDL_SetWindowBordered(g_window, SDL_TRUE);
    PsyX_ApplyWindowState(g_PcConfig.windowWidth, g_PcConfig.windowHeight, g_PcConfig.fullscreen);
    s_windowArranged = 0;
}

int Pc_Split_Start(int padK, int playAs, int port, const char* server, int code, const char** err)
{
    static char cmd[1600];
    char        exe[512];
    char        guid[64];
    char        name[32];
    SDL_Rect    disp, r1, r2;
    int         di, dev, n, k;
    const char* me;

    if (err)
        *err = NULL;
    if (s_role == 2)
    {
        if (err) *err = "This is Player 2's window.";
        return 0;
    }
    dev = Split_PadDevice(padK);
    if (dev < 0)
    {
        if (err) *err = "Plug in a controller for Player 2 first.";
        return 0;
    }
    if (!g_window || !PcProc_ExePath(exe, sizeof(exe)))
    {
        if (err) *err = "Could not find the game exe.";
        return 0;
    }
    if (s_child)
    {
        /* A previous attempt's window. */
        PcProc_Kill(s_child);
        s_child = NULL;
    }

    di = SDL_GetWindowDisplayIndex(g_window);
    if (di < 0)
        di = 0;
    if (SDL_GetDisplayBounds(di, &disp) != 0)
    {
        disp.x = 0; disp.y = 0; disp.w = 1920; disp.h = 1080;
    }
    if (g_PcConfig.splitLayout == 1) /* side by side */
    {
        r1.x = disp.x;              r1.y = disp.y; r1.w = disp.w / 2;          r1.h = disp.h;
        r2.x = disp.x + disp.w / 2; r2.y = disp.y; r2.w = disp.w - disp.w / 2; r2.h = disp.h;
    }
    else /* Player 1 above, Player 2 below */
    {
        r1.x = disp.x; r1.y = disp.y;              r1.w = disp.w; r1.h = disp.h / 2;
        r2.x = disp.x; r2.y = disp.y + disp.h / 2; r2.w = disp.w; r2.h = disp.h - disp.h / 2;
    }

    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(dev), guid, sizeof(guid));
    me = g_PcConfig.onlineName[0] ? g_PcConfig.onlineName : "Player";
    snprintf(name, sizeof(name), "%.16s 2", me);
    for (k = 0; name[k]; k++)
        if (name[k] == '"') name[k] = '\'';

    n = snprintf(cmd, sizeof(cmd),
                 "\"%s\" -split2 -pad %d -padguid %s -window %d,%d,%d,%d -port %d -parent %lu -name \"%s\" -playas %d -p2audio %d",
                 exe, dev, guid, r2.x, r2.y, r2.w, r2.h, port, PcProc_SelfPid(), name, playAs, g_PcConfig.splitP2Audio);
    if (n > 0 && n < (int)sizeof(cmd) && server && server[0])
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " -server \"%s\" -room %d", server, code);
    if (n > 0 && n < (int)sizeof(cmd) && s_dataArg[0])
        snprintf(cmd + n, sizeof(cmd) - (size_t)n, " -data \"%s\"", s_dataArg);

    s_child = PcProc_Spawn(cmd);
    if (!s_child)
    {
        if (err) *err = "Could not launch Player 2's window.";
        SH_DBG("[SPLIT] launch failed: %s", cmd);
        return 0;
    }
    SH_DBG("[SPLIT] launched Player 2: %s", cmd);

    Split_ArrangeOwnWindow(&r1);
    /* Player 2's pad is theirs now; every other pad, the keyboard and the mouse
     * stay Player 1's. */
    PsyX_Pad_SetDeviceFilter(2, dev);

    s_role          = 1;
    s_hostWaiting   = 1;
    s_hostWaitSince = SDL_GetTicks();
    s_sessionSeen   = 0;
    s_childStopping = 0;
    snprintf(s_status, sizeof(s_status), "Starting Player 2's window...");
    if (!s_atExitSet)
    {
        atexit(Split_AtExit);
        s_atExitSet = 1;
    }
    return 1;
}

int Pc_Split_HostWaiting(void)
{
    return s_role == 1 && s_hostWaiting;
}

void Pc_Split_HostStarted(void)
{
    s_hostWaiting = 0;
    SH_DBG("[SPLIT] Player 2 is in, starting the game");
}

void Pc_Split_StatusText(char* out, int cap)
{
    if (out && cap > 0)
        snprintf(out, cap, "%s", s_status);
}

void Pc_Split_Stop(void)
{
    if (s_role != 1)
        return;
    if (s_child)
    {
        /* Player 2's copy quits on its own when the room closes; it is killed
         * from Pc_Split_Tick if it has not gone after a grace period. */
        s_childStopping = 1;
        s_childStopMs   = SDL_GetTicks();
    }
    PsyX_Pad_SetDeviceFilter(0, -1);
    Split_RestoreOwnWindow();
    s_hostWaiting = 0;
    s_role        = 0;
    SH_DBG("[SPLIT] back to one window");
}

/* ------------------------------------------------------------------ */
/* Player 2                                                            */
/* ------------------------------------------------------------------ */

/* The second window shares the room with the first, so its music would play
 * twice a few milliseconds apart. Effects stay: they are positional, from
 * Player 2's spot. The stock volume is re-applied whenever the game resets it
 * (new game, load, options). */
static void Split_ApplyP2Audio(void)
{
    if (s_argAudio == 1)
        return;
    if (s_argAudio == 2)
    {
        if (g_GameWork.config.volumeBgm != OPT_SOUND_VOLUME_MIN || g_GameWork.config.volumeSe != OPT_SOUND_VOLUME_MIN)
        {
            g_GameWork.config.volumeBgm = OPT_SOUND_VOLUME_MIN;
            g_GameWork.config.volumeSe  = OPT_SOUND_VOLUME_MIN;
            Sd_SetVolume(OPT_SOUND_VOLUME_MIN, OPT_SOUND_VOLUME_MIN, OPT_SOUND_VOLUME_MIN);
        }
        return;
    }
    if (g_GameWork.config.volumeBgm != OPT_SOUND_VOLUME_MIN)
    {
        g_GameWork.config.volumeBgm = OPT_SOUND_VOLUME_MIN;
        Sd_SetVolume(OPT_SOUND_VOLUME_MAX, OPT_SOUND_VOLUME_MIN, g_GameWork.config.volumeSe);
    }
}

static void Split_Quit(const char* why)
{
    SH_DBG("[SPLIT] Player 2 window quitting: %s", why);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Per frame                                                           */
/* ------------------------------------------------------------------ */

void Pc_Split_Tick(void)
{
    unsigned int now = SDL_GetTicks();

    /* A room on the server launches Player 2's window once it has its code;
     * with Online off the game starts once Player 2 is in. */
    Pc_CoopMenu_SplitAutoStart();

    if (s_child && s_childStopping)
    {
        if (!PcProc_Alive(s_child) || (int)(now - s_childStopMs) > 3000)
        {
            PcProc_Kill(s_child);
            s_child         = NULL;
            s_childStopping = 0;
        }
    }

    if (s_role == 1)
    {
        if (s_child && !PcProc_Alive(s_child))
        {
            PcProc_Kill(s_child);
            s_child = NULL;
            SH_DBG("[SPLIT] Player 2's window closed");
            Pc_Split_Stop();
            return;
        }
        if (ShSession_Active())
            s_sessionSeen = 1;
        else if (s_sessionSeen || (int)(now - s_hostWaitSince) > 15000)
        {
            /* Left to the title, or the room never opened (the port is in use):
             * one window again. */
            SH_DBG("[SPLIT] room %s", s_sessionSeen ? "closed" : "never opened");
            Pc_Split_Stop();
            return;
        }
        if (s_hostWaiting)
        {
            if (ShSession_Active() && ShSession_MemberCount() >= 2)
                Pc_Split_HostStarted(); /* Player 2 is in; an online room waits in the lobby */
            if (s_hostWaiting && (int)(now - s_hostWaitSince) > 90000)
                snprintf(s_status, sizeof(s_status), "Player 2 did not connect. Close the room and try again.");
            else if (s_hostWaiting && ShSession_Active() && (int)(now - s_hostWaitSince) > 4000)
                snprintf(s_status, sizeof(s_status), "Waiting for Player 2's window to join...");
        }
    }
    else if (s_role == 2)
    {
        if (s_argParent && (int)(now - s_nextParentCheckMs) >= 0)
        {
            s_nextParentCheckMs = now + 2000;
            if (!PcProc_PidAlive(s_argParent))
                Split_Quit("Player 1's game is gone");
        }
        if (ShSession_Active())
        {
            s_wasActive = 1;
        }
        else if (s_wasActive)
        {
            Split_Quit("the room closed");
        }
        else if (g_GameWork.gameState == GameState_MainMenu)
        {
            if (!s_menuOpened)
            {
                Pc_CoopMenu_OpenSplitGuest();
                s_menuOpened = 1;
                s_nextJoinMs = now;
            }
            if ((int)(now - s_nextJoinMs) >= 0)
            {
                int port = s_argPort ? s_argPort : SPLIT_DEFAULT_PORT;
                s_nextJoinMs = now + 3000;
                s_joinTries++;
                if (s_argServer[0])
                {
                    /* Player 1 hosts on their server: the same room, by its code. */
                    ShSession_RequestJoinServer(s_argServer, port, s_argRoom, 1);
                    SH_DBG("[SPLIT] joining Player 1's room %d on %s:%d (try %d)", s_argRoom, s_argServer, port, s_joinTries);
                }
                else
                {
                    ShSession_RequestJoinServer("127.0.0.1", port, 0, 0);
                    SH_DBG("[SPLIT] joining Player 1's room on 127.0.0.1:%d (try %d)", port, s_joinTries);
                }
            }
        }
        Split_ApplyP2Audio();
    }
}
