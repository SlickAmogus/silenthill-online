/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_menu.c - the simple co-op front end (state + logic only).
 *
 * Rendering lives in sh_net_ui.c (Nu_DrawCoopMenu), so this menu shares the
 * clean panel look of the rest of the online UI and draws from the same
 * post-capture hook on the title screen and in game alike. This file only holds
 * the page/selection state, services input, and exposes accessors the renderer
 * reads. Host / Join drive the Steam session layer (sh_net_session.h).
 */

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_coop_menu.h"
#include "pc_coop_save.h"
#include "pc_config.h"
#include "pc_playas.h" /* the Character row */
#include "pc_split_screen.h"
#include "pc_mouse_cursor.h" /* hover/click hit-test in Update, like the other windows */
#include "sh_net.h"
#include "sh_net_session.h"
#include "sh_net_invite.h" /* the Invite / Accept / Decline rows */
#include "sh_net_chat.h"   /* the Chat row */
#include "sh_net_proto.h" /* SHNET_PERM_* */
#include "sh_net_steam.h"
#include "sh_log.h"
#include "bodyprog/sound/sound_system.h" /* Sd_PlaySfx: the same menu sounds the title uses */
#include "bodyprog/sound/sfx_id_enum.h"

#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <stdlib.h>
#include <SDL.h>

const char* ShNet_LocalName(void);

typedef enum
{
    COOP_PAGE_ROOT = 0,     /* main-menu: Host / Join / Back */
    COOP_PAGE_HOST_SETUP,
    COOP_PAGE_SPLIT,        /* split screen: Player 2's pad, character, layout, sound, load, online, start */
    COOP_PAGE_SPLIT_ONLINE, /* the online room's settings for a split-screen game */
    COOP_PAGE_HOST,
    COOP_PAGE_JOIN,
    COOP_PAGE_ROOMS,        /* server room browser */
    COOP_PAGE_INGAME,       /* in-game M menu: Resume / prefs / players / leave */
    COOP_PAGE_PLAYERS,      /* member list (in-game) */
    COOP_PAGE_PLAYER,       /* one member: allow cheats / remove (host) */
    COOP_PAGE_INVITE,       /* accepting an invite: save first, or go */
    COOP_PAGE_SAVES,        /* Start From: every save to pick from */
    COOP_PAGE_GAMEOVER,     /* host, after a co-op game over: Yes / Load Game / Quit Without Saving */
    COOP_PAGE_SETTINGS      /* phones and the browser: server, living world, name */
} CoopPage;

static unsigned long long s_playerId; /* the member COOP_PAGE_PLAYER is about */
static char               s_playerName[SHSESSION_NAME_MAX];

static int      s_open;
static int      s_inGame;   /* 1 = opened in-game (M menu), 0 = main-menu popup */
static CoopPage s_page;
static int      s_sel;
static char     s_msg[64];  /* transient feedback shown on the in-game status line */

/* The row under the pointer, handed over by the renderer each frame; -1 none. */
static int      s_hover = -1;

/* 1 while the CURRENT game was launched as multiplayer (host or join via the
 * Multiplayer menu). This -- not a config flag -- is what turns on the in-game
 * co-op features (the M menu, no pausing). A normal New Game / Continue leaves
 * it 0, so single-player is identical to the stock port. title.c sets it at the
 * boot and clears it at the menu. */
int g_PcCoopGame;

/* Pending host settings, seeded from config when the setup page is entered and
 * written back to config just before the lobby is opened (the worker reads them
 * there -- see ShSession_Tick's wantHost handler). */
static int s_setMaxPlayers = 4;
static int s_setPublic     = 0;
static int s_setFps        = 60;
static int s_setLiving     = 0; /* part of the living world, or private */
/* Where to host: 0 = this PC (relay runs in-process), 1 = my configured server,
 * 2 = Steam. This PC is the default -- the lowest-setup way to host. */
#define COOP_HOST_THISPC 0
#define COOP_HOST_SERVER 1
#define COOP_HOST_STEAM  2
#if defined(__EMSCRIPTEN__)
/* A browser reaches a server over WebSocket and nothing else: it cannot run
 * a relay, take a direct connection or talk to Steam. */
#define COOP_WEB 1
#else
#define COOP_WEB 0
#endif
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
/* A phone has one screen and no second pad to give Player 2; the browser
 * build has no second window to open for Player 2 either. */
#define COOP_MOBILE 1
#else
#define COOP_MOBILE 0
#endif
/* The server, the living world switch and the name, in the game itself: the
 * only place to set them on a phone or in a browser, and on PC the same
 * settings the launcher's Online page writes, changed without leaving. */
#define COOP_HAS_SETTINGS 1

#define COOP_DEFAULT_SERVER "server.sh1pc.com"

enum { COOP_ROOT_HOST, COOP_ROOT_JOIN, COOP_ROOT_SPLIT, COOP_ROOT_SETTINGS, COOP_ROOT_BACK };

/* The title-menu rows this platform shows, in order. */
static int Coop_RootRows(int* rows)
{
    int n = 0;
    rows[n++] = COOP_ROOT_HOST;
    rows[n++] = COOP_ROOT_JOIN;
    if (!COOP_MOBILE && !COOP_WEB) rows[n++] = COOP_ROOT_SPLIT;
    if (COOP_HAS_SETTINGS) rows[n++] = COOP_ROOT_SETTINGS;
    rows[n++] = COOP_ROOT_BACK;
    return n;
}

static int Coop_RootRow(int i)
{
    int rows[5];
    int n = Coop_RootRows(rows);
    return (i >= 0 && i < n) ? rows[i] : COOP_ROOT_BACK;
}
static int s_hostMode = COOP_WEB ? COOP_HOST_SERVER : COOP_HOST_THISPC;

/* Split screen (pc_split_screen.c): Player 2's controller (index among the
 * attached ones) and character. The layout and what their window plays are
 * config (split_layout, split_p2_audio). */
static int s_splitPad    = 0;
static int s_splitPlayAs = -1;
/* Online for a split-screen game: 0 = off (the room is still on this PC, so a
 * friend can join by IP, and the game starts as soon as Player 2 is in);
 * 1 = hosted on this PC, 2 = on my server. 1 and 2 wait in the lobby for
 * Start Game. Steam cannot carry a split-screen game: Player 2's window has
 * no Steam identity of its own. */
#define COOP_SPLIT_OFF    0
#define COOP_SPLIT_THISPC 1
#define COOP_SPLIT_SERVER 2
static int s_splitOnline = COOP_SPLIT_OFF;
/* Server: Player 2's window is launched once the room has its code. */
static int          s_splitLaunch;
static unsigned int s_splitLaunchMs;

/* The co-op relay server: the configured online server, or loopback. */
static const char* Coop_ServerIp(void)
{
    const char* ip = g_PcConfig.onlineServer;
    return (ip && ip[0]) ? ip : "127.0.0.1";
}
static int Coop_ServerPort(void)
{
    return g_PcConfig.onlinePort ? g_PcConfig.onlinePort : 27888;
}

/* Ask the configured server for its open rooms so the browser is ready. Skipped
 * when we are already on a transport -- a list request repoints the co-op client
 * and would drop a room we are hosting or in. */
static void Coop_RefreshServerList(void)
{
    if (ShSession_Active()) return;
    if (!g_PcConfig.onlineServer[0]) return;
    ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort(), 1);
}

/* ------------------------------------------------------------------ */
/* Text entry (Join by IP / room code)                                 */
/* ------------------------------------------------------------------ */
/* A single editable field fed by SDL text-input events, which is what a
 * phone's on-screen keyboard sends (polling the key state misses its key
 * presses, which come and go between two frames). SDL_StartTextInput is what
 * brings that keyboard up. While a field is active Pc_CoopMenu_Update ignores
 * navigation so typed keys never move the selection; Enter (or a tap on the
 * menu) commits, Esc or Back cancels.
 *
 * On Android SDL delivers those events from the Java thread, so the watch
 * hands them over through a spinlocked buffer that the menu drains. */
#define COOP_EDIT_NONE   0
#define COOP_EDIT_IP     1   /* host[:port] */
#define COOP_EDIT_CODE   2   /* 4-digit room code on the configured server */
#define COOP_EDIT_SERVER 3   /* the living world's server (settings page) */
#define COOP_EDIT_NAME   4   /* multiplayer name (settings page) */

static int           s_editKind;
static char          s_editBuf[80];
static int           s_editLen;
static int           s_editHaveKeys;
static int           s_editStartedText; /* we turned text input on, so we turn it off */
static unsigned int  s_editEndMs;
static int           s_clickPrevHeld;
static int           s_pressRow = -1; /* row the pointer went down on; -1 none */

static SDL_SpinLock  s_typedLock;
static char          s_typed[64];
static int           s_typedLen;
static int           s_typedBack, s_typedEnter, s_typedEsc;
static int           s_watching;

static int SDLCALL Coop_TextWatch(void* user, SDL_Event* e)
{
    (void)user;
    if (s_editKind == COOP_EDIT_NONE)
    {
        return 0;
    }
    SDL_AtomicLock(&s_typedLock);
    if (e->type == SDL_TEXTINPUT)
    {
        const char* t = e->text.text;
        for (; *t && s_typedLen < (int)sizeof(s_typed); t++)
        {
            s_typed[s_typedLen++] = *t;
        }
    }
    else if (e->type == SDL_KEYDOWN)
    {
        SDL_Scancode sc = e->key.keysym.scancode;
        if (sc == SDL_SCANCODE_BACKSPACE) s_typedBack++;
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) s_typedEnter = 1;
        else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK) s_typedEsc = 1;
    }
    SDL_AtomicUnlock(&s_typedLock);
    return 0;
}

int Pc_CoopMenu_Editing(void)
{
    return s_editKind != COOP_EDIT_NONE;
}

static void Coop_BeginEdit(int kind)
{
    const char* start = "";
    if (kind == COOP_EDIT_SERVER) start = g_PcConfig.onlineServer;
    if (kind == COOP_EDIT_NAME)   start = g_PcConfig.onlineName;
    if (!s_watching)
    {
        SDL_AddEventWatch(Coop_TextWatch, NULL);
        s_watching = 1;
    }
    SDL_AtomicLock(&s_typedLock);
    s_typedLen = s_typedBack = s_typedEnter = s_typedEsc = 0;
    SDL_AtomicUnlock(&s_typedLock);
    SDL_strlcpy(s_editBuf, start, sizeof(s_editBuf));
    s_editLen      = (int)strlen(s_editBuf);
    s_editKind     = kind;
    s_editHaveKeys = 0; /* swallow the press that opened the field */
    /* On a phone, starting text input is also what raises the on-screen
     * keyboard, and SDL keeps calling text input "active" after the keyboard
     * was dismissed (the Back key, a tap elsewhere), so skipping the call when
     * it already counted as active left the field with no keyboard at all. */
    s_editStartedText = SDL_HasScreenKeyboardSupport() || !SDL_IsTextInputActive();
    if (s_editStartedText)
    {
        SDL_StartTextInput();
    }
    SH_DBG("[COOP] text field %d opened (screen keyboard %s)", kind,
           SDL_HasScreenKeyboardSupport() ? "raised" : "not used");
}

static void Coop_EndEdit(void)
{
    /* Typing is not keyboard navigation: bring the pointer straight back
     * rather than leave it hidden until the mouse moves. */
    extern int g_PsyX_PadInputLast;
    g_PsyX_PadInputLast = 0;
    s_editKind  = COOP_EDIT_NONE;
    s_editEndMs = SDL_GetTicks();
    if (s_editStartedText)
    {
        SDL_StopTextInput();
        s_editStartedText = 0;
    }
}

/* Whether `c` may be typed into the current field. */
static int Coop_EditAccepts(char c)
{
    if ((unsigned char)c < 32 || (unsigned char)c > 126) return 0;
    switch (s_editKind)
    {
    case COOP_EDIT_CODE:   return c >= '0' && c <= '9';
    case COOP_EDIT_IP:
    case COOP_EDIT_SERVER: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  c == '.' || c == ':' || c == '-' || c == '/' || c == '_';
    case COOP_EDIT_NAME:   return s_editLen < SHNET_NAME_MAX - 1;
    default:               return 0;
    }
}

static void Coop_CommitEdit(void)
{
    if (s_editKind == COOP_EDIT_IP && s_editLen > 0)
    {
        /* host[:port]; default port when none is typed. */
        char host[80];
        int  port = Coop_ServerPort();
        char* colon;
        SDL_strlcpy(host, s_editBuf, sizeof(host));
        colon = strrchr(host, ':');
        if (colon)
        {
            *colon = '\0';
            if (colon[1]) port = atoi(colon + 1);
        }
        if (host[0])
        {
            ShSession_RequestJoinServer(host, port, 0, 0); /* direct IP; 0 = the host's room */
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
    }
    else if (s_editKind == COOP_EDIT_CODE && s_editLen > 0)
    {
        int code = atoi(s_editBuf);
        if (code > 0)
        {
            ShSession_RequestJoinServer(Coop_ServerIp(), Coop_ServerPort(), code, 1);
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
    }
    else if (s_editKind == COOP_EDIT_SERVER)
    {
        SDL_strlcpy(g_PcConfig.onlineServer, s_editLen > 0 ? s_editBuf : COOP_DEFAULT_SERVER,
                    sizeof(g_PcConfig.onlineServer));
        PcConfig_SaveKeyValue("online_server", g_PcConfig.onlineServer);
        ShNet_ApplySettings();
    }
    else if (s_editKind == COOP_EDIT_NAME)
    {
        /* Spaces at either end come free with a phone keyboard's word
         * suggestions; the name is what is between them. */
        char  name[sizeof(s_editBuf)];
        char* p = s_editBuf;
        int   n;
        while (*p == ' ') p++;
        SDL_strlcpy(name, p, sizeof(name));
        n = (int)strlen(name);
        while (n > 0 && name[n - 1] == ' ') name[--n] = '\0';
        if (n > 0)
        {
            SDL_strlcpy(g_PcConfig.onlineName, name, sizeof(g_PcConfig.onlineName));
            PcConfig_SaveKeyValue("online_name", g_PcConfig.onlineName);
            ShNet_ApplySettings();
            SH_DBG("[COOP] name set to \"%s\"", g_PcConfig.onlineName);
        }
    }
    Coop_EndEdit();
}

void Pc_CoopMenu_FeedKeys(const unsigned char* ks)
{
    char typed[64];
    int  n, back, enter, esc, i;

    (void)ks;
    if (s_editKind == COOP_EDIT_NONE)
    {
        return;
    }
    if (!s_editHaveKeys) /* the press that opened the field is not typed */
    {
        SDL_AtomicLock(&s_typedLock);
        s_typedLen = s_typedBack = s_typedEnter = s_typedEsc = 0;
        SDL_AtomicUnlock(&s_typedLock);
        s_editHaveKeys = 1;
        return;
    }

    SDL_AtomicLock(&s_typedLock);
    n = s_typedLen;
    memcpy(typed, s_typed, (size_t)n);
    back  = s_typedBack;
    enter = s_typedEnter;
    esc   = s_typedEsc;
    s_typedLen = s_typedBack = s_typedEnter = s_typedEsc = 0;
    SDL_AtomicUnlock(&s_typedLock);

    if (esc)
    {
        Coop_EndEdit();
        return;
    }
    while (back-- > 0 && s_editLen > 0)
    {
        s_editBuf[--s_editLen] = '\0';
    }
    for (i = 0; i < n; i++)
    {
        if (Coop_EditAccepts(typed[i]) && s_editLen < (int)sizeof(s_editBuf) - 1)
        {
            s_editBuf[s_editLen++] = typed[i];
            s_editBuf[s_editLen]   = '\0';
        }
    }
    /* A tap commits too: a phone keyboard's Done key does not always send
     * Enter, and there is no other way to say "finished". */
    if (enter || Pc_MouseCursor_LeftClicked())
    {
        Coop_CommitEdit();
    }
}

/* Save picker for the host setup and split screen: the host's own
 * single-player saves and which one to start from (-1 = a fresh New Game).
 * Guests pick nothing: they play the host's game. */
static CoopSaveEntry s_saves[60];
static int           s_saveCount;
static int           s_loadIdx = -1;

/* Start-game request handed to title.c (GameState_MainMenu_Update), which does
 * the actual boot. 0 = none, 1 = new game, 2 = load s_startSave. */
static int  s_startReq;
static char s_startSave[COOP_SAVE_KEY_MAX];

static void Coop_RefreshSaves(void)
{
    s_saveCount = Pc_CoopSave_List(s_saves, (int)(sizeof(s_saves) / sizeof(s_saves[0])));
    s_loadIdx   = -1;
    SH_DBG("[COOPSAVE] %d save(s) offered for Start From", s_saveCount);
}

/* New Game, then the saves newest first; wraps both ways. */
static void Coop_StepSave(int dir)
{
    s_loadIdx += dir;
    if (s_loadIdx >= s_saveCount) s_loadIdx = -1;
    if (s_loadIdx < -1)           s_loadIdx = s_saveCount - 1;
}

static void Coop_CycleSave(void)
{
    Coop_StepSave(1);
}

/* Just where: the time, the number and the rest are on the Start From list,
 * which Enter or a tap on this row opens. */
static void Coop_SaveRowText(char* out, int cap)
{
    if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
        snprintf(out, cap, "Start From:  %s", s_saves[s_loadIdx].where);
    else
        snprintf(out, cap, "Start From:  New Game");
}

/* The Start From list, opened from a row of `from`; it comes back there. */
static int s_savesFrom, s_savesFromRow;

/* The co-op game over prompt: whether Continue has a game to boot, and
 * whether the host has chosen (until the title takes the start request). */
static int s_goCanContinue, s_goDecided;
static int Coop_GoRow(int i) { return s_goCanContinue ? i : i + 1; } /* 0 Yes, 1 Load Game, 2 Quit */
/* The save list opened to load a game now (the game over, or the host's
 * in-game Load Game) rather than to pick the lobby's Start From. */
static int Coop_SavesLoadNow(void) { return s_savesFrom == COOP_PAGE_GAMEOVER || s_savesFrom == COOP_PAGE_INGAME; }

static void Coop_OpenSaves(int from, int row)
{
    s_savesFrom    = from;
    s_savesFromRow = row;
    s_page         = COOP_PAGE_SAVES;
    s_sel          = s_loadIdx + 1;
    s_msg[0]       = '\0';
}

static void Coop_CloseSaves(void)
{
    s_page = s_savesFrom;
    s_sel  = s_savesFromRow;
}

void Pc_CoopMenu_SaveEntry(int row, char* line1, int cap1, char* line2, int cap2, char* right, int capR)
{
    const CoopSaveEntry* e;
    if (row <= 0 || row > s_saveCount)
    {
        snprintf(line1, (size_t)cap1, "New Game");
        snprintf(line2, (size_t)cap2, "Start from the beginning");
        if (capR > 0) right[0] = '\0';
        return;
    }
    e = &s_saves[row - 1];
    snprintf(line1, (size_t)cap1, "%s", e->where);
    snprintf(right, (size_t)capR, "%d:%02d:%02d", e->secs / 3600, (e->secs / 60) % 60, e->secs % 60);
    {
        const char* diff = e->difficulty < 0 ? "Easy" : e->difficulty > 0 ? "Hard" : "Normal";
        char        extra[48] = "";
        if (e->nextFear)
            snprintf(extra, sizeof(extra), " - Next Fear");
        else if (e->clears > 0)
            snprintf(extra, sizeof(extra), " - %d clear%s", e->clears, e->clears == 1 ? "" : "s");
        /* Short enough for one line at the small size: no scrolling. */
        snprintf(line2, (size_t)cap2, "Save %d - %s%s - %s", e->saveNo, diff, extra, e->card);
    }
}

void Pc_CoopMenu_OpenGameOver(int canContinue)
{
    s_open          = 1;
    s_inGame        = 1;
    s_page          = COOP_PAGE_GAMEOVER;
    s_sel           = 0;
    s_msg[0]        = '\0';
    s_goCanContinue = canContinue;
    SH_DBG("[COOP] game over prompt (continue %s)", canContinue ? "available" : "not available");
}

int Pc_CoopMenu_GameOverDecided(void)
{
    return s_goDecided;
}

/* The game over's Yes and Load Game, and the in-game Load Game, reboot
 * through the title as the co-op host, and the guests are told to start from
 * what we load (SHNET_CTRL_RESTART). */
static void Coop_HostReloadBoot(void)
{
    extern void ShNetG_RestartAfterBoot(void);
    s_goDecided  = 1;
    g_PcCoopGame = 1;
    ShNetG_RestartAfterBoot();
    g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    Pc_CoopMenu_Close();
}

void Pc_CoopMenu_Open(void)
{
    s_open   = 1;
    s_inGame = 0;
    s_page   = COOP_PAGE_ROOT;
    s_sel    = 0;
    SH_DBG("[COOP] menu opened");
}

void Pc_CoopMenu_OpenInGame(void)
{
    s_open   = 1;
    s_inGame = 1;
    s_page   = COOP_PAGE_INGAME;
    s_sel    = 0;
    s_msg[0] = '\0';
    SH_DBG("[COOP] in-game M menu opened");
}

int Pc_CoopMenu_InGame(void)
{
    return s_open && s_inGame;
}

void Pc_CoopMenu_Close(void)
{
    s_open = 0;
}

int Pc_CoopMenu_IsOpen(void)
{
    return s_open;
}

static void Coop_EnterHostSetup(void)
{
    s_setMaxPlayers = g_PcConfig.onlineSteamMaxPlayers;
    if (s_setMaxPlayers < 2) s_setMaxPlayers = 2;
    if (s_setMaxPlayers > 4) s_setMaxPlayers = 4;
    s_setPublic   = 1; /* "Room List: Listed" by default so friends can browse to you */
    s_setFps      = 60; /* co-op poses stream every frame; 60 is the smooth default */
    s_setLiving   = g_PcConfig.onlineCoopLiving ? 1 : 0; /* private unless the host chose otherwise */
    Coop_RefreshSaves(); /* default: start a fresh game */
    s_page        = COOP_PAGE_HOST_SETUP;
    s_sel         = 0;
}

/* Queue the game boot for title.c. Reads the host-setup load choice. */
static void Coop_RequestStart(void)
{
    if (s_loadIdx >= 0 && s_loadIdx < s_saveCount)
    {
        s_startReq = 2;
        snprintf(s_startSave, sizeof(s_startSave), "%s", s_saves[s_loadIdx].key);
    }
    else
    {
        s_startReq      = 1;
        s_startSave[0]  = '\0';
    }
    Pc_CoopMenu_Close();
}

/* A guest moves to another map the way a co-op save boots: the current
 * savegame (inventory, flags) with the destination written in becomes the
 * autosave, the game resets to the title, and title.c boots it as a
 * Continue (start request 3) and puts us beside the host once his pose is
 * heard (g_PcCoopGuestSpawn). */
void Pc_CoopMenu_RequestWarp(int mapIdx, int x, int z, short rotY)
{
    if (!g_PcCoopGame || !ShSession_Active() || g_SavegamePtr == NULL) return;
    g_GameWork.autosave                 = *g_SavegamePtr;
    g_GameWork.autosave.mapIdx          = mapIdx;
    g_GameWork.autosave.playerPositionX = x;
    g_GameWork.autosave.playerPositionZ = z;
    g_GameWork.autosave.playerRotationY = rotY;
    if (g_GameWork.autosave.playerHealth <= 0)
    {
        g_GameWork.autosave.playerHealth = Q12(100.0f);
    }
    s_startReq = 3;
    g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    Pc_CoopMenu_Close();
    SH_DBG("[COOP] warping to map %d", mapIdx);
}

/* The host loaded a save: everyone starts from it. The host's savegame
 * becomes our autosave and the game reboots into it the way a warp does,
 * landing next to the host at the save point. */
void Pc_CoopMenu_RequestReboot(const void* savegame)
{
    if (!g_PcCoopGame || !ShSession_Active() || g_SavegamePtr == NULL || savegame == NULL) return;
    g_GameWork.autosave = *(const s_Savegame*)savegame;
    if (g_GameWork.autosave.playerHealth <= 0)
    {
        g_GameWork.autosave.playerHealth = Q12(100.0f);
    }
    s_startReq = 3;
    g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    Pc_CoopMenu_Close();
    SH_DBG("[COOP] rebooting into the host's save (map %d)", (int)g_GameWork.autosave.mapIdx);
}

/* A living-world player accepted our invite: a private room on the living
 * world server, which the connection we already have carries. Their invite
 * came through the living world, so the shared game stays part of it. */
int Pc_CoopMenu_InviteHost(void)
{
    int max = g_PcConfig.onlineSteamMaxPlayers;
    if (!g_PcConfig.onlineServer[0] || (ShSession_Active() && !ShSession_IsHost())) return 0;
    if (max < 2) max = 2;
    if (max > 4) max = 4;
    g_PcConfig.onlineCoopLiving = 1;
    PcConfig_SaveKeyValue("online_coop_living_world", "1");
    s_hostMode = COOP_HOST_SERVER;
    ShSession_RequestHostServer(Coop_ServerIp(), Coop_ServerPort(), max, 1, 1);
    SH_DBG("[COOP] invite: opening a private room for %d players", max);
    return 1;
}

void Pc_CoopMenu_InviteJoin(int code)
{
    if (!g_PcConfig.onlineServer[0] || ShSession_Active()) return;
    ShSession_RequestJoinServer(Coop_ServerIp(), Coop_ServerPort(), code, 1);
}

/* Our running single-player game continues as the co-op host game: the live
 * savegame, with where we stand written in, boots as a Continue (start
 * request 4) exactly the way a warp does, and comes back with g_PcCoopGame
 * set so every co-op hook starts the way it does from the lobby. */
void Pc_CoopMenu_RequestHostReboot(void)
{
    if (g_SavegamePtr == NULL || g_GameWork.gameState != GameState_InGame) return;
    /* A save made on the spot, the way a save point makes one: the live
     * player state goes into the savegame first, then into the autosave the
     * Continue boots. */
    SysWork_SavegameUpdatePlayer();
    g_GameWork.autosave = *g_SavegamePtr;
    if (g_GameWork.autosave.playerHealth <= 0)
    {
        g_GameWork.autosave.playerHealth = Q12(100.0f);
    }
    g_PcCoopGame = 1;
    s_startReq   = 4;
    g_SysWork.sysFlags |= SysFlag_DoWarmReset;
    Pc_CoopMenu_Close();
    SH_DBG("[COOP] invite accepted: rebooting as the co-op host (map %d)", (int)g_GameWork.autosave.mapIdx);
}

int Pc_CoopMenu_TakeStartRequest(char* outName, int cap)
{
    int req = s_startReq;
    s_goDecided = 0; /* the title is up: the game over's choice is made */
    s_startReq = 0;
    if (outName && cap > 0)
    {
        snprintf(outName, cap, "%s", s_startSave);
    }
    return req;
}

static void Coop_StartHosting(void)
{
    /* The worker reads these out of config when it creates the lobby. */
    g_PcConfig.onlineSteamMaxPlayers = s_setMaxPlayers;
    g_PcConfig.onlineSteamPublic     = s_setPublic;
    g_PcConfig.fpsCap                = s_setFps;
    g_PcConfig.onlineCoopLiving      = s_setLiving;
    PcConfig_SaveKeyValue("online_coop_living_world", s_setLiving ? "1" : "0");
    SH_DBG("[COOP] host mode %d: %d players, %s, fps %d",
           s_hostMode, s_setMaxPlayers, s_setPublic ? "public" : "private", s_setFps);
    if (s_hostMode == COOP_HOST_STEAM)
    {
        ShSession_RequestHost();
    }
    else if (s_hostMode == COOP_HOST_SERVER)
    {
        ShSession_RequestHostServer(Coop_ServerIp(), Coop_ServerPort(), s_setMaxPlayers, !s_setPublic, 1);
    }
    else /* this PC: run the relay in-process */
    {
        ShSession_RequestHostListen(Coop_ServerPort(), s_setMaxPlayers);
    }
    s_page = COOP_PAGE_HOST;
    s_sel  = 0;
}

static void Coop_EnterSplit(void)
{
    Coop_RefreshSaves();
    s_setMaxPlayers = g_PcConfig.onlineSteamMaxPlayers;
    if (s_setMaxPlayers < 2) s_setMaxPlayers = 2;
    if (s_setMaxPlayers > 4) s_setMaxPlayers = 4;
    s_setPublic = 1;
    s_setFps    = 60;
    s_setLiving = g_PcConfig.onlineCoopLiving ? 1 : 0;
    s_splitLaunch = 0;
    if (s_splitPlayAs < 0 || s_splitPlayAs >= Pc_PlayAs_Count())
        s_splitPlayAs = (Pc_PlayAs_Current() + 1) % Pc_PlayAs_Count(); /* someone other than Player 1 */
    if (s_splitPad >= Pc_Split_PadCount())
        s_splitPad = 0;
    s_msg[0] = '\0';
    s_page   = COOP_PAGE_SPLIT;
    s_sel    = 0;
}

/* Start: a room (on this PC's relay, as "Host on: This PC" opens, so a friend
 * can still join over the network; or on my server), then Player 2's window,
 * which joins it. With Online off the lobby starts the game by itself once
 * Player 2 is in; otherwise it waits for Start Game like any hosted room. */
static void Coop_StartSplit(void)
{
    const char* err = NULL;

    if (Pc_Split_PadCount() == 0)
    {
        snprintf(s_msg, sizeof(s_msg), "Plug in a controller for Player 2 first.");
        return;
    }
    if (s_splitOnline == COOP_SPLIT_SERVER && !g_PcConfig.onlineServer[0])
    {
        snprintf(s_msg, sizeof(s_msg), "No server is configured (online_server in config.cfg).");
        return;
    }
    SH_DBG("[COOP] split screen: pad %d, character %d, layout %d, p2 audio %d, online %d",
           s_splitPad, s_splitPlayAs, g_PcConfig.splitLayout, g_PcConfig.splitP2Audio, s_splitOnline);
    g_PcConfig.onlineSteamMaxPlayers = s_setMaxPlayers;
    g_PcConfig.onlineSteamPublic     = s_setPublic;
    g_PcConfig.fpsCap                = s_setFps;
    g_PcConfig.onlineCoopLiving      = s_setLiving;
    if (s_splitOnline == COOP_SPLIT_SERVER)
    {
        /* Player 2's window needs the room's code, which the server hands
         * out once the room is open: launched from Pc_CoopMenu_SplitAutoStart. */
        ShSession_RequestHostServer(Coop_ServerIp(), Coop_ServerPort(), s_setMaxPlayers, !s_setPublic, 1);
        s_splitLaunch   = 1;
        s_splitLaunchMs = SDL_GetTicks();
        s_page = COOP_PAGE_HOST;
        s_sel  = 0;
        return;
    }
    ShSession_RequestHostListen(Coop_ServerPort(), s_splitOnline ? s_setMaxPlayers : 4);
    if (!Pc_Split_Start(s_splitPad, s_splitPlayAs, Coop_ServerPort(), NULL, 0, &err))
    {
        snprintf(s_msg, sizeof(s_msg), "%s", err ? err : "Could not start split screen.");
        ShSession_RequestLeave();
        return;
    }
    s_page = COOP_PAGE_HOST;
    s_sel  = 0;
}

void Pc_CoopMenu_SplitAutoStart(void)
{
    if (s_splitLaunch)
    {
        const char*        err = NULL;
        unsigned long long id  = ShSession_LobbyId();
        if (ShSession_Active() && ShSession_IsHost() && id && id < 100000ull)
        {
            s_splitLaunch = 0;
            if (!Pc_Split_Start(s_splitPad, s_splitPlayAs, Coop_ServerPort(), Coop_ServerIp(), (int)id, &err))
            {
                snprintf(s_msg, sizeof(s_msg), "%s", err ? err : "Could not start split screen.");
                ShSession_RequestLeave();
                s_page = COOP_PAGE_SPLIT;
                s_sel  = 0;
            }
        }
        else if ((int)(SDL_GetTicks() - s_splitLaunchMs) > 20000)
        {
            s_splitLaunch = 0;
            snprintf(s_msg, sizeof(s_msg), "The server did not open a room.");
            ShSession_RequestLeave();
            s_page = COOP_PAGE_SPLIT;
            s_sel  = 0;
        }
        return;
    }
    if (s_splitOnline == COOP_SPLIT_OFF && Pc_Split_HostWaiting() &&
        ShSession_Active() && ShSession_IsHost() && ShSession_MemberCount() >= 2)
    {
        Pc_Split_HostStarted();
        Coop_RequestStart();
    }
}

void Pc_CoopMenu_OpenSplitGuest(void)
{
    s_open   = 1;
    s_inGame = 0;
    s_page   = COOP_PAGE_JOIN;
    s_sel    = 0;
    s_msg[0] = '\0';
    SH_DBG("[COOP] split screen Player 2: lobby opened");
}

/* The in-game page. The host's rules (Friendly Fire, Guest Cheats) are the
 * host's rows only; a guest gets Teleport to Host in their place. */
enum
{
    COOP_IG_RESUME = 0, COOP_IG_MESSAGE, COOP_IG_SAVE, COOP_IG_CHARACTER, COOP_IG_NAMEPLATES,
    COOP_IG_HEALTHBARS, COOP_IG_FRIENDLY, COOP_IG_CHEATS, COOP_IG_TELEPORT, COOP_IG_PLAYERS, COOP_IG_LEAVE,
    COOP_IG_INVITE, COOP_IG_INVITE_CANCEL, COOP_IG_ACCEPT, COOP_IG_DECLINE, COOP_IG_CHAT, COOP_IG_LOAD
};
#define COOP_IG_MAX 20

static int Coop_InGameRows(int* rows)
{
    int guest = ShSession_Active() && !ShSession_IsHost();
    int n = 0;
    rows[n++] = COOP_IG_RESUME;
    rows[n++] = COOP_IG_MESSAGE;
    /* Living-world invites: an answer owed, an invite out, or someone to ask. */
    if (ShNetInvite_Incoming(NULL, 0))
    {
        rows[n++] = COOP_IG_ACCEPT;
        rows[n++] = COOP_IG_DECLINE;
    }
    else if (ShNetInvite_Outgoing(NULL, 0))
    {
        rows[n++] = COOP_IG_INVITE_CANCEL;
    }
    else if (ShNetInvite_CanInvite() && ShNetInvite_Nearest(NULL, 0, NULL))
    {
        rows[n++] = COOP_IG_INVITE;
    }
    rows[n++] = COOP_IG_CHAT;
    /* Outside a co-op game (the living world alone) that is the whole menu:
     * the rest belongs to a shared game. */
    if (!g_PcCoopGame)
    {
        return n;
    }
    rows[n++] = COOP_IG_SAVE;
    if (!guest)
    {
        rows[n++] = COOP_IG_LOAD; /* the way out of a soft lock, for everyone */
    }
    rows[n++] = COOP_IG_CHARACTER;
    rows[n++] = COOP_IG_NAMEPLATES;
    rows[n++] = COOP_IG_HEALTHBARS;
    if (guest)
    {
        rows[n++] = COOP_IG_TELEPORT;
    }
    else
    {
        rows[n++] = COOP_IG_FRIENDLY;
        rows[n++] = COOP_IG_CHEATS;
    }
    rows[n++] = COOP_IG_PLAYERS;
    rows[n++] = COOP_IG_LEAVE;
    return n;
}

static int Coop_InGameRow(int i)
{
    int r[COOP_IG_MAX];
    int n = Coop_InGameRows(r);
    return (i >= 0 && i < n) ? r[i] : COOP_IG_LEAVE;
}

/* Where Players sits on the in-game page (the row Back returns to). */
static int Coop_PlayersRow(void)
{
    int r[COOP_IG_MAX];
    int n = Coop_InGameRows(r), i;
    for (i = 0; i < n; i++)
    {
        if (r[i] == COOP_IG_PLAYERS) return i;
    }
    return 0;
}

/* Rows on the current page; the last row is always Back. */
int Pc_CoopMenu_RowCount(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:       { int r[5]; return Coop_RootRows(r); } /* Host, Join, [Split Screen,] [Settings,] Back */
    case COOP_PAGE_SETTINGS:   return 7; /* Living World IP, Living World, Name, Join Messages, Chat, Restart Connection, Back */
    case COOP_PAGE_HOST_SETUP: return 8; /* Host on, Players, Visibility, Living World, FPS, Load, Open, Back */
    case COOP_PAGE_SPLIT:      return s_splitOnline ? 9 : 8; /* Pad, Character, Layout, Sound, Load, Online, [Online Settings,] Start, Back */
    case COOP_PAGE_SPLIT_ONLINE: return 5; /* Players, Visibility, Living World, FPS, Back */
    case COOP_PAGE_HOST:       return (ShSession_Active() && !ShSession_IsHost()) ? 3 : 5; /* guest: Character, Leave, Back; host: Invite/Code, Character, Start, Close, Back */
    case COOP_PAGE_JOIN:       return 5; /* Browse, Join by IP, Enter code, Steam, Back */
    case COOP_PAGE_ROOMS:      return ShSession_RoomCount() + 2; /* rooms + Refresh + Back */
    case COOP_PAGE_INGAME:     { int r[COOP_IG_MAX]; return Coop_InGameRows(r); }
    case COOP_PAGE_PLAYERS:    return ShSession_MemberCount() + 1; /* members + Back */
    case COOP_PAGE_PLAYER:     return 4; /* Allow Cheats, Bring Here, Remove, Back */
    case COOP_PAGE_INVITE:     return 3; /* Save First, Join Without Saving, Back */
    case COOP_PAGE_SAVES:      return s_saveCount + 2; /* New Game, the saves, Back */
    case COOP_PAGE_GAMEOVER:   return s_goCanContinue ? 3 : 2; /* [Yes,] Load Game, Quit Without Saving */
    default:                   return 1;
    }
}

int Pc_CoopMenu_View(void)
{
    switch (s_page)
    {
    case COOP_PAGE_HOST_SETUP: return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_SPLIT:      return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_SPLIT_ONLINE: return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_INGAME:     return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_INVITE:     return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_GAMEOVER:   return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_SETTINGS:   return PC_COOP_VIEW_SETTINGS;
    case COOP_PAGE_ROOMS:      return PC_COOP_VIEW_ROOMS;
    case COOP_PAGE_PLAYERS:    return PC_COOP_VIEW_PLAYERS;
    case COOP_PAGE_SAVES:      return PC_COOP_VIEW_SAVES;
    default:                   return PC_COOP_VIEW_MENU;
    }
}

int Pc_CoopMenu_IsTopLevel(void)
{
    return s_page == COOP_PAGE_ROOT || s_page == COOP_PAGE_INGAME;
}

int Pc_CoopMenu_Selected(void)
{
    return s_sel;
}

const char* Pc_CoopMenu_Title(void)
{
    switch (s_page)
    {
    case COOP_PAGE_HOST_SETUP: return "HOST A GAME";
    case COOP_PAGE_SPLIT:      return "SPLIT SCREEN";
    case COOP_PAGE_SPLIT_ONLINE: return "ONLINE SETTINGS";
    case COOP_PAGE_HOST:       return "LOBBY";
    case COOP_PAGE_JOIN:       return "JOIN A GAME";
    case COOP_PAGE_ROOMS:      return "GAMES";
    case COOP_PAGE_INGAME:     return "MULTIPLAYER";
    case COOP_PAGE_PLAYERS:    return "PLAYERS";
    case COOP_PAGE_PLAYER:     return s_playerName;
    case COOP_PAGE_INVITE:     return "ACCEPT INVITE";
    case COOP_PAGE_GAMEOVER:   return "GAME OVER";
    case COOP_PAGE_SAVES:      return Coop_SavesLoadNow() ? "LOAD GAME" : "START FROM";
    case COOP_PAGE_SETTINGS:   return "MULTIPLAYER SETTINGS";
    default:                   return "MULTIPLAYER";
    }
}

void Pc_CoopMenu_RowText(int i, char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        switch (Coop_RootRow(i))
        {
        case COOP_ROOT_HOST:     snprintf(out, cap, "Host Game"); break;
        case COOP_ROOT_JOIN:     snprintf(out, cap, "Join Game"); break;
        case COOP_ROOT_SPLIT:    snprintf(out, cap, "Split Screen"); break;
        case COOP_ROOT_SETTINGS: snprintf(out, cap, "Multiplayer Settings"); break;
        default:                 snprintf(out, cap, "Back"); break;
        }
        break;

    case COOP_PAGE_SETTINGS:
        if (i == 0)      snprintf(out, cap, "Living World IP:  %s", g_PcConfig.onlineServer[0] ? g_PcConfig.onlineServer : COOP_DEFAULT_SERVER);
        else if (i == 1) snprintf(out, cap, "Living World:  %s", g_PcConfig.onlineEnabled ? "On" : "Off");
        else if (i == 2) snprintf(out, cap, "Name:  %s", ShNet_LocalName());
        else if (i == 3) snprintf(out, cap, "Join Messages:  %s", g_PcConfig.onlineJoinsHidden ? "Hidden" : "Shown");
        else if (i == 4) snprintf(out, cap, "Chat:  %s", ShNetChat_Hidden() ? "Hidden" : "Shown");
        else if (i == 5) snprintf(out, cap, "Restart Connection");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST_SETUP:
        if (i == 0)
        {
            const char* where = (s_hostMode == COOP_HOST_STEAM)  ? "Steam"
                              : (s_hostMode == COOP_HOST_SERVER)  ? "Server"
                                                                  : "This PC";
            snprintf(out, cap, "Host on:  %s", where);
        }
        else if (i == 1) snprintf(out, cap, "Max Players:  %d", s_setMaxPlayers);
        else if (i == 2) snprintf(out, cap, "Room List:  %s", s_setPublic ? "Listed" : "Hidden (code only)");
        else if (i == 3) snprintf(out, cap, "Living World:  %s", s_setLiving ? "Shown" : "Private");
        else if (i == 4) snprintf(out, cap, "FPS Lock:  %d", s_setFps);
        else if (i == 5) Coop_SaveRowText(out, cap);
        else if (i == 6) snprintf(out, cap, s_hostMode == COOP_HOST_STEAM ? "Open Lobby" : "Open Room");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_SPLIT:
        if (i == 0)      snprintf(out, cap, "2P Pad:  %s", Pc_Split_PadName(s_splitPad));
        else if (i == 1) snprintf(out, cap, "2P Character:  %s", Pc_PlayAs_Label(s_splitPlayAs));
        else if (i == 2) snprintf(out, cap, "Layout:  %s", g_PcConfig.splitLayout ? "Side by Side" : "Top and Bottom");
        else if (i == 3)
        {
            static const char* const au[3] = { "Effects Only", "Everything", "Muted" };
            snprintf(out, cap, "2P Sound:  %s", au[g_PcConfig.splitP2Audio % 3]);
        }
        else if (i == 4) Coop_SaveRowText(out, cap);
        else if (i == 5)
        {
            const char* on = (s_splitOnline == COOP_SPLIT_SERVER) ? "Server"
                           : (s_splitOnline == COOP_SPLIT_THISPC) ? "This PC" : "Off";
            snprintf(out, cap, "Online:  %s", on);
        }
        else if (s_splitOnline && i == 6) snprintf(out, cap, "Online Settings");
        else if (i == (s_splitOnline ? 7 : 6)) snprintf(out, cap, "Start Split Screen");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_SPLIT_ONLINE:
        if (i == 0)      snprintf(out, cap, "Max Players:  %d", s_setMaxPlayers);
        else if (i == 1) snprintf(out, cap, "Room List:  %s", s_setPublic ? "Listed" : "Hidden (code only)");
        else if (i == 2) snprintf(out, cap, "Living World:  %s", s_setLiving ? "Shown" : "Private");
        else if (i == 3) snprintf(out, cap, "FPS Lock:  %d", s_setFps);
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_HOST:
        if (ShSession_Active() && !ShSession_IsHost())
        {
            /* Guest lobby: pick a character, wait or leave. */
            if (i == 0)      snprintf(out, cap, "Character:  %s", Pc_PlayAs_Label(Pc_PlayAs_Current()));
            else if (i == 1) snprintf(out, cap, "Leave Room");
            else             snprintf(out, cap, "Back");
            break;
        }
        if (i == 0)
        {
            /* A server room has a shareable code; a Steam lobby has an invite;
             * a room on this PC (its code is the local relay's, no use to a
             * friend) is reached by this machine's address. */
            unsigned long long id = ShSession_LobbyId();
            if (s_hostMode == COOP_HOST_THISPC) snprintf(out, cap, "Friends join by your IP, UDP %d", Coop_ServerPort());
            else if (id && id < 100000ull)     snprintf(out, cap, "Room Code:  %u", (unsigned)id);
            else                               snprintf(out, cap, "Invite Friend");
        }
        else if (i == 1) snprintf(out, cap, "Character:  %s", Pc_PlayAs_Label(Pc_PlayAs_Current()));
        else if (i == 2) snprintf(out, cap, "Start Game");
        else if (i == 3) snprintf(out, cap, "Close Room");
        else snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_INGAME:
        switch (Coop_InGameRow(i))
        {
        case COOP_IG_RESUME:     snprintf(out, cap, "Resume"); break;
        case COOP_IG_MESSAGE:    snprintf(out, cap, "Leave a Message"); break;
        case COOP_IG_SAVE:       snprintf(out, cap, "Save"); break;
        case COOP_IG_LOAD:       snprintf(out, cap, "Load Game"); break;
        case COOP_IG_CHARACTER:  snprintf(out, cap, "Character:  %s", Pc_PlayAs_Label(Pc_PlayAs_Current())); break;
        case COOP_IG_NAMEPLATES: snprintf(out, cap, "Nameplates:  %s", g_PcConfig.onlineNameplates ? "On" : "Off"); break;
        case COOP_IG_CHAT:       snprintf(out, cap, "Chat:  %s", ShNetChat_Hidden() ? "Hidden" : "Shown"); break;
        case COOP_IG_HEALTHBARS:
        {
            static const char* const hb[4] = { "Off", "Mine", "Partners", "Everyone" };
            snprintf(out, cap, "Health Bars:  %s", hb[g_PcConfig.onlineHealthBars & 3]);
            break;
        }
        case COOP_IG_FRIENDLY:   snprintf(out, cap, "Friendly Fire:  %s", ShNet_CoopFriendlyFire() ? "On" : "Off"); break;
        case COOP_IG_CHEATS:     snprintf(out, cap, "Guest Cheats:  %s", ShNet_CoopGuestConsole() ? "On" : "Off"); break;
        case COOP_IG_TELEPORT:   snprintf(out, cap, "Teleport to Host"); break;
        case COOP_IG_PLAYERS:    snprintf(out, cap, "Players"); break;
        case COOP_IG_INVITE:
        {
            char who[SHNET_NAME_MAX];
            int  cool = 0;
            ShNetInvite_Nearest(who, (int)sizeof(who), &cool);
            if (ShNetInvite_Blocked()) snprintf(out, cap, "Invite %s  (not during a boss fight)", who);
            else if (cool > 0)         snprintf(out, cap, "Invite %s  (declined, %d s)", who, cool);
            else                       snprintf(out, cap, "Invite %s", who);
            break;
        }
        case COOP_IG_INVITE_CANCEL:
        {
            char who[SHNET_NAME_MAX];
            int  left = ShNetInvite_Outgoing(who, (int)sizeof(who));
            if (ShNetInvite_OutgoingSaving()) snprintf(out, cap, "Cancel Invite to %s  (saving)", who);
            else                              snprintf(out, cap, "Cancel Invite to %s  (%d s)", who, left);
            break;
        }
        case COOP_IG_ACCEPT:
        {
            char who[SHNET_NAME_MAX];
            int  left = ShNetInvite_Incoming(who, (int)sizeof(who));
            if (ShNetInvite_Blocked()) snprintf(out, cap, "Accept %s's Invite  (not during a boss fight)", who);
            else                       snprintf(out, cap, "Accept %s's Invite  (%d s)", who, left);
            break;
        }
        case COOP_IG_DECLINE:    snprintf(out, cap, "Decline Invite"); break;
        default:                 snprintf(out, cap, "Leave to Title"); break;
        }
        break;

    case COOP_PAGE_PLAYERS:
    {
        int mc = ShSession_MemberCount();
        if (i < mc)
        {
            const ShSessionMember* m = ShSession_Member(i);
            if (m && m->pingMs >= 0)
                snprintf(out, cap, "%s  (%dms)", m->name, m->pingMs);
            else if (m)
                snprintf(out, cap, "%s", m->name);
        }
        else
        {
            snprintf(out, cap, "Back");
        }
        break;
    }

    case COOP_PAGE_SAVES:
        if (i <= s_saveCount)
        {
            char l2[96], r[16];
            Pc_CoopMenu_SaveEntry(i, out, cap, l2, (int)sizeof(l2), r, (int)sizeof(r));
        }
        else
            snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_GAMEOVER:
        switch (Coop_GoRow(i))
        {
        case 0:  snprintf(out, cap, "Yes"); break;
        case 1:  snprintf(out, cap, "Load Game"); break;
        default: snprintf(out, cap, "Quit Without Saving"); break;
        }
        break;

    case COOP_PAGE_INVITE:
        if (i == 0)      snprintf(out, cap, "Save First");
        else if (i == 1) snprintf(out, cap, "Join Without Saving");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_PLAYER:
        if (i == 0)      snprintf(out, cap, "Allow Cheats:  %s", (ShNet_CoopPermGet(s_playerId) & SHNET_PERM_CHEATS) ? "Yes" : "No");
        else if (i == 1) snprintf(out, cap, "Bring Here");
        else if (i == 2) snprintf(out, cap, "Remove From Game");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_JOIN:
        if (i == 0)      snprintf(out, cap, "Browse Server Rooms");
        else if (i == 1) snprintf(out, cap, "Join by IP...");
        else if (i == 2) snprintf(out, cap, "Enter Room Code...");
        else if (i == 3) snprintf(out, cap, "Join a Friend (Steam)");
        else             snprintf(out, cap, "Back");
        break;

    case COOP_PAGE_ROOMS:
    {
        int rc = ShSession_RoomCount();
        if (i < rc)
        {
            int  code = 0, players = 0, max = 0, map = -1;
            char host[SHSESSION_NAME_MAX];
            host[0] = '\0';
            ShSession_RoomGet(i, &code, &players, &max, &map, host, (int)sizeof(host));
            snprintf(out, cap, "%s's Game  #%d  (%d/%d)", host[0] ? host : "Someone", code, players, max);
        }
        else if (i == rc) snprintf(out, cap, "Refresh");
        else              snprintf(out, cap, "Back");
        break;
    }

    default:
        snprintf(out, cap, "Back");
        break;
    }
}

void Pc_CoopMenu_StatusText(char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (s_editKind == COOP_EDIT_IP)
    {
        snprintf(out, cap, "Server IP:  %s_", s_editBuf);
        return;
    }
    if (s_editKind == COOP_EDIT_CODE)
    {
        snprintf(out, cap, "Room Code:  %s_", s_editBuf);
        return;
    }
    if (s_editKind == COOP_EDIT_SERVER)
    {
        snprintf(out, cap, "Living World IP:  %s_", s_editBuf);
        return;
    }
    if (s_editKind == COOP_EDIT_NAME)
    {
        snprintf(out, cap, "Name:  %s_", s_editBuf);
        return;
    }
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        if (!ShSteam_Available() && !COOP_HAS_SETTINGS)
        {
            snprintf(out, cap, "Steam not running - start Steam and relaunch.");
        }
        break;

    case COOP_PAGE_SETTINGS:
        if (s_msg[0]) snprintf(out, cap, "%s", s_msg);
        else          snprintf(out, cap, "%s", g_PcConfig.onlineEnabled ? ShNet_StatusText() : "Living world off.");
        break;


    case COOP_PAGE_HOST:
        if (s_splitLaunch)
        {
            snprintf(out, cap, "Opening the room on the server...");
            break;
        }
        if (Pc_Split_HostWaiting())
        {
            Pc_Split_StatusText(out, cap);
            break;
        }
        ShSession_StatusLine(out, cap);
        if (!ShSession_IsHost() && ShSession_Active())
        {
            char live[128];
            SDL_strlcpy(live, out, sizeof(live));
            snprintf(out, cap, "%s   -   waiting for the host to start", live);
        }
        else if (s_hostMode == COOP_HOST_STEAM && ShSteam_Available() && !ShSteam_OverlayEnabled())
        {
            /* The one thing that makes Steam invites fail silently. */
            snprintf(out, cap, "Steam overlay off: add the game to Steam (non-Steam game) and launch it there for invites.");
        }
        break;

    case COOP_PAGE_JOIN:
        if (Pc_Split_Role() == 2)
        {
            snprintf(out, cap, "Connecting to Player 1's game...");
            break;
        }
        if (ShSteam_LastError()[0])
        {
            snprintf(out, cap, "Steam: %s", ShSteam_LastError());
        }
        else if (ShSteam_LobbyState() == SHSTEAM_LOBBY_IN)
        {
            /* Already in a friend's lobby: tell them whether it is still
             * gathering or already a game in progress (you drop straight in). */
            const char* st = ShSteam_GetLobbyData("state");
            if (st && strcmp(st, "ingame") == 0)
            {
                snprintf(out, cap, "Game in progress - dropping you in...");
            }
            else
            {
                snprintf(out, cap, "In the lobby - waiting for the host to start.");
            }
        }
        else
        {
            snprintf(out, cap, "Accept a Steam invite from a friend to join.");
        }
        break;

    case COOP_PAGE_ROOMS:
        if (s_msg[0])                       { snprintf(out, cap, "%s", s_msg); }
        else if (ShSteam_LastError()[0])    { snprintf(out, cap, "Steam: %s", ShSteam_LastError()); }
        else if (ShSession_RoomCount() > 0) { ShSession_StatusLine(out, cap); }
        else                                { snprintf(out, cap, "No open games - pick Refresh, or Host one."); }
        break;

    case COOP_PAGE_INGAME:
        if (s_msg[0])            { snprintf(out, cap, "%s", s_msg); }
        else if (!g_PcCoopGame)  { snprintf(out, cap, "Living world:  %s", ShNet_StatusText()); }
        else                     { ShSession_StatusLine(out, cap); }
        break;

    case COOP_PAGE_SAVES:
        if (s_saveCount == 0)
            snprintf(out, cap, "No saves found in gamedata/save. Start a New Game, or save in single player first.");
        else
            snprintf(out, cap, "%d save%s, newest first. %s", s_saveCount, s_saveCount == 1 ? "" : "s",
                     Coop_SavesLoadNow() ? "Everyone starts again from the one you pick."
                                         : "Pick one to start the game from.");
        break;

    case COOP_PAGE_GAMEOVER:
        if (s_goCanContinue)
            snprintf(out, cap, "Do you want to continue?  Everyone starts again from where you continue.");
        else
            snprintf(out, cap, "There is no game to continue. Load a save?  Everyone starts again from it.");
        break;

    case COOP_PAGE_INVITE:
    {
        char who[SHNET_NAME_MAX];
        int  left = ShNetInvite_Incoming(who, (int)sizeof(who));
        if (s_msg[0])  snprintf(out, cap, "%s", s_msg);
        else if (left) snprintf(out, cap, "Your own game ends when you join %s. Save it first?  (%d s)", who, left);
        else           snprintf(out, cap, "The invite is no longer open.");
        break;
    }

    case COOP_PAGE_HOST_SETUP:
        if (s_hostMode == COOP_HOST_THISPC)
            snprintf(out, cap, "This PC hosts. Friends join with your IP (forward UDP %d).", Coop_ServerPort());
        else if (s_hostMode == COOP_HOST_SERVER)
            snprintf(out, cap, "A room on your server (%s). Friends browse or enter the code.", Coop_ServerIp());
        else
            snprintf(out, cap, "Opens a Steam lobby; invite friends from the overlay.");
        break;

    case COOP_PAGE_SPLIT:
        if (s_msg[0])                      snprintf(out, cap, "%s", s_msg);
        else if (Pc_Split_PadCount() == 0) snprintf(out, cap, "No controller found. Plug one in for Player 2.");
        else                               snprintf(out, cap, "Player 1 keeps the keyboard, mouse and other pads. Player 2 gets the pad above.");
        break;

    case COOP_PAGE_PLAYERS:
        if (s_msg[0])                 snprintf(out, cap, "%s", s_msg);
        else if (ShSession_IsHost())  snprintf(out, cap, "Select a player to remove them from the game.");
        break;

    default:
        break;
    }
}

/* The ordinary save screen, so a multiplayer save lands on the memory card
 * beside the single-player ones. Feedback for a refusal goes to s_msg. */
static void Coop_DoSave(void)
{
    if (Pc_Save_BossActive())
    {
        snprintf(s_msg, sizeof(s_msg), "Can't save during a boss fight");
        return;
    }
    Pc_CoopMenu_Close();
    if (!Pc_Save_OpenSaveScreen())
    {
        Pc_CoopMenu_OpenInGame();
        snprintf(s_msg, sizeof(s_msg), "Can't save right now");
    }
}

static void Coop_Confirm(void)
{
    switch (s_page)
    {
    case COOP_PAGE_ROOT:
        switch (Coop_RootRow(s_sel))
        {
        case COOP_ROOT_HOST:     Coop_EnterHostSetup(); break;
        case COOP_ROOT_JOIN:     s_page = COOP_PAGE_JOIN; s_sel = 0; Coop_RefreshServerList(); break;
        case COOP_ROOT_SPLIT:    Coop_EnterSplit(); break;
        case COOP_ROOT_SETTINGS: s_page = COOP_PAGE_SETTINGS; s_sel = 0; s_msg[0] = '\0'; break;
        default:                 Pc_CoopMenu_Close(); break;
        }
        break;

    case COOP_PAGE_SETTINGS:
        s_msg[0] = '\0';
        if (s_sel == 0)      { Coop_BeginEdit(COOP_EDIT_SERVER); }
        else if (s_sel == 1)
        {
            g_PcConfig.onlineEnabled = !g_PcConfig.onlineEnabled;
            PcConfig_SaveKeyValue("online_enabled", g_PcConfig.onlineEnabled ? "1" : "0");
            ShNet_ApplySettings();
        }
        else if (s_sel == 2) { Coop_BeginEdit(COOP_EDIT_NAME); }
        else if (s_sel == 3)
        {
            g_PcConfig.onlineJoinsHidden = !g_PcConfig.onlineJoinsHidden;
            PcConfig_SaveKeyValue("online_joins_hidden", g_PcConfig.onlineJoinsHidden ? "1" : "0");
        }
        else if (s_sel == 4) { ShNetChat_SetHidden(!ShNetChat_Hidden()); }
        else if (s_sel == 5) { ShNet_ApplySettings(); } /* the status line follows the connection from here */
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_HOST_SETUP:
        /* Setting rows cycle on confirm; no left/right needed. */
        if (s_sel == 0)      { if (!COOP_WEB) s_hostMode = (s_hostMode + 1) % 3; }
        else if (s_sel == 1) { s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1; }
        else if (s_sel == 2) { s_setPublic = !s_setPublic; }
        else if (s_sel == 3) { s_setLiving = !s_setLiving; }
        else if (s_sel == 4) { s_setFps = (s_setFps == 60) ? 30 : 60; }
        else if (s_sel == 5) { Coop_OpenSaves(COOP_PAGE_HOST_SETUP, 5); }
        else if (s_sel == 6) { Coop_StartHosting(); } /* Open Room / Lobby */
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_SPLIT:
        if (s_sel == 0)
        {
            int n = Pc_Split_PadCount();
            s_splitPad = (n > 0) ? (s_splitPad + 1) % n : 0;
        }
        else if (s_sel == 1) { s_splitPlayAs = (s_splitPlayAs + 1) % Pc_PlayAs_Count(); }
        else if (s_sel == 2)
        {
            g_PcConfig.splitLayout = !g_PcConfig.splitLayout;
            PcConfig_SaveKeyValue("split_layout", g_PcConfig.splitLayout ? "1" : "0");
        }
        else if (s_sel == 3)
        {
            char v[4];
            g_PcConfig.splitP2Audio = (g_PcConfig.splitP2Audio + 1) % 3;
            snprintf(v, sizeof(v), "%d", g_PcConfig.splitP2Audio);
            PcConfig_SaveKeyValue("split_p2_audio", v);
        }
        else if (s_sel == 4) { Coop_OpenSaves(COOP_PAGE_SPLIT, 4); }
        else if (s_sel == 5) { s_splitOnline = (s_splitOnline + 1) % 3; }
        else if (s_splitOnline && s_sel == 6) { s_page = COOP_PAGE_SPLIT_ONLINE; s_sel = 0; }
        else if (s_sel == (s_splitOnline ? 7 : 6)) { Coop_StartSplit(); }
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_SPLIT_ONLINE:
        if (s_sel == 0)      { s_setMaxPlayers = (s_setMaxPlayers >= 4) ? 2 : s_setMaxPlayers + 1; }
        else if (s_sel == 1) { s_setPublic = !s_setPublic; }
        else if (s_sel == 2) { s_setLiving = !s_setLiving; }
        else if (s_sel == 3) { s_setFps = (s_setFps == 60) ? 30 : 60; }
        else                 { s_page = COOP_PAGE_SPLIT; s_sel = 6; }
        break;

    case COOP_PAGE_HOST:
        if (ShSession_Active() && !ShSession_IsHost())
        {
            if (s_sel == 0)      { Pc_PlayAs_Cycle(1); break; }           /* Character */
            else if (s_sel == 1) { ShSession_RequestLeave(); }            /* Leave Room */
            s_page = COOP_PAGE_ROOT; s_sel = 0;
            break;
        }
        if (s_sel == 0)      { ShSession_RequestInvite(); }               /* Steam invite; the room code is informational */
        else if (s_sel == 1) { Pc_PlayAs_Cycle(1); }                      /* Character */
        else if (s_sel == 2) { Coop_RequestStart(); }                     /* Start Game */
        else if (s_sel == 3) { ShSession_RequestLeave(); Pc_Split_Stop(); s_page = COOP_PAGE_ROOT; s_sel = 0; } /* Close Room */
        else                 { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_INGAME:
        switch (Coop_InGameRow(s_sel))
        {
        case COOP_IG_RESUME: Pc_CoopMenu_Close(); break;
        case COOP_IG_MESSAGE:
        {
            extern void ShNetMemo_ComposerToggle(void);
            if (!ShNet_CoopLivingWorld())
            {
                snprintf(s_msg, sizeof(s_msg), "Messages are off: this game is private (host's Living World setting).");
                break;
            }
            Pc_CoopMenu_Close();         /* get the menu out of the way */
            ShNetMemo_ComposerToggle();  /* open the living-world message composer */
            break;
        }
        case COOP_IG_SAVE:       Coop_DoSave(); break;
        case COOP_IG_LOAD:       Coop_RefreshSaves(); Coop_OpenSaves(COOP_PAGE_INGAME, s_sel); break;
        case COOP_IG_CHARACTER:  Pc_PlayAs_Cycle(1); break; /* reloads the body in place */
        case COOP_IG_NAMEPLATES: g_PcConfig.onlineNameplates = !g_PcConfig.onlineNameplates; break;
        case COOP_IG_CHAT:       ShNetChat_SetHidden(!ShNetChat_Hidden()); break;
        case COOP_IG_HEALTHBARS:
        {
            char v[4];
            g_PcConfig.onlineHealthBars = (g_PcConfig.onlineHealthBars + 1) & 3;
            snprintf(v, sizeof(v), "%d", g_PcConfig.onlineHealthBars);
            PcConfig_SaveKeyValue("online_health_bars", v);
            break;
        }
        case COOP_IG_FRIENDLY:
            /* One rule for the whole game, set by the world's owner (the row
             * is the host's only; a guest's menu has no such row). */
            if (!ShSession_Active() || ShSession_IsHost())
                g_PcConfig.onlineFriendlyFire = !g_PcConfig.onlineFriendlyFire;
            break;
        case COOP_IG_CHEATS:
            if (!ShSession_Active() || ShSession_IsHost())
            {
                g_PcConfig.onlineGuestConsole = !g_PcConfig.onlineGuestConsole;
                PcConfig_SaveKeyValue("online_guest_console", g_PcConfig.onlineGuestConsole ? "1" : "0");
            }
            break;
        case COOP_IG_TELEPORT:
        {
            extern void ShNetCut_TeleportToHost(void);
            ShNetCut_TeleportToHost();
            Pc_CoopMenu_Close();
            break;
        }
        case COOP_IG_PLAYERS: s_page = COOP_PAGE_PLAYERS; s_sel = 0; break;
        case COOP_IG_INVITE:
            ShNetInvite_Send();
            if (!ShNetInvite_Blocked()) Pc_CoopMenu_Close();
            break;
        case COOP_IG_INVITE_CANCEL: ShNetInvite_Cancel();  s_sel = 0; break;
        case COOP_IG_ACCEPT:
            if (ShNetInvite_Blocked()) ShNetInvite_Accept(); /* says why */
            else { s_page = COOP_PAGE_INVITE; s_sel = 0; s_msg[0] = '\0'; }
            break;
        case COOP_IG_DECLINE:       ShNetInvite_Decline(); s_sel = 0; break;
        default:
            /* Leave to Title without saving. */
            ShSession_RequestLeave();
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            Pc_CoopMenu_Close();
            break;
        }
        break;

    case COOP_PAGE_PLAYERS:
        if (s_sel < ShSession_MemberCount())
        {
            /* Host: a page for that player (allow cheats, remove). */
            const ShSessionMember* m = ShSession_Member(s_sel);
            if (!ShSession_IsHost() || !m || m->steamId == ShSession_SelfId())
            {
                break;
            }
            s_playerId = m->steamId;
            SDL_strlcpy(s_playerName, m->name, sizeof(s_playerName));
            s_page = COOP_PAGE_PLAYER;
            s_sel  = 0;
            s_msg[0] = '\0';
            break;
        }
        /* Back returns to the in-game root (the Players row). */
        s_page = COOP_PAGE_INGAME;
        s_sel  = Coop_PlayersRow();
        s_msg[0] = '\0';
        break;

    case COOP_PAGE_SAVES:
        if (s_sel <= s_saveCount)
        {
            s_loadIdx = s_sel - 1; /* row 0 is New Game */
            if (Coop_SavesLoadNow())
            {
                SH_DBG("[COOP] host loads %s for everyone", s_loadIdx >= 0 ? s_saves[s_loadIdx].key : "a new game");
                Coop_RequestStart();
                Coop_HostReloadBoot();
                break;
            }
        }
        Coop_CloseSaves();
        break;

    case COOP_PAGE_GAMEOVER:
        switch (Coop_GoRow(s_sel))
        {
        case 0:
            /* The title's Continue, booted as the co-op host. */
            SH_DBG("[COOP] game over: continue");
            s_startReq = 4;
            Coop_HostReloadBoot();
            break;
        case 1:
            Coop_RefreshSaves();
            Coop_OpenSaves(COOP_PAGE_GAMEOVER, s_sel);
            break;
        default:
            SH_DBG("[COOP] game over: quit without saving");
            s_goDecided = 1;
            ShSession_RequestLeave();
            g_SysWork.sysFlags |= SysFlag_DoWarmReset;
            Pc_CoopMenu_Close();
            break;
        }
        break;

    case COOP_PAGE_INVITE:
        if (!ShNetInvite_Incoming(NULL, 0) || s_sel == 2)
        {
            s_page = COOP_PAGE_INGAME;
            s_sel  = 0;
            s_msg[0] = '\0';
        }
        else if (s_sel == 0)
        {
            Pc_CoopMenu_Close();
            if (!ShNetInvite_AcceptAfterSave())
            {
                Pc_CoopMenu_OpenInGame();
                s_page = COOP_PAGE_INVITE;
                snprintf(s_msg, sizeof(s_msg), "Can't save right now");
            }
        }
        else
        {
            ShNetInvite_Accept();
            if (!ShNetInvite_Blocked()) Pc_CoopMenu_Close();
        }
        break;

    case COOP_PAGE_PLAYER:
        if (s_sel == 0)
        {
            int f = ShNet_CoopPermGet(s_playerId) ^ SHNET_PERM_CHEATS;
            ShNet_CoopPermSet(s_playerId, f);
            snprintf(s_msg, sizeof(s_msg), "%s %s use cheats and the console.", s_playerName,
                     (f & SHNET_PERM_CHEATS) ? "may now" : "may no longer");
        }
        else if (s_sel == 1)
        {
            extern void ShNetCut_Summon(unsigned long long id);
            ShNetCut_Summon(s_playerId);
            snprintf(s_msg, sizeof(s_msg), "Bringing %s here.", s_playerName);
        }
        else if (s_sel == 2)
        {
            ShSession_RequestKick(s_playerId);
            snprintf(s_msg, sizeof(s_msg), "Removed %s.", s_playerName);
            s_page = COOP_PAGE_PLAYERS;
            s_sel  = 0;
        }
        else
        {
            s_page = COOP_PAGE_PLAYERS;
            s_sel  = 0;
        }
        break;

    case COOP_PAGE_JOIN:
        if (s_sel == 0) /* browse the server's open rooms */
        {
            ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort(), 1);
            s_page = COOP_PAGE_ROOMS;
            s_sel  = 0;
        }
        else if (s_sel == 1) /* type host[:port] */
        {
            if (COOP_WEB) snprintf(s_msg, sizeof(s_msg), "In the browser, join a room on the server.");
            else          Coop_BeginEdit(COOP_EDIT_IP);
        }
        else if (s_sel == 2) { Coop_BeginEdit(COOP_EDIT_CODE); } /* type a room code */
        else if (s_sel == 3)
        {
            /* Steam: an invite is accepted in Steam itself (the overlay or the
             * friends list's Join Game); this row explains that and opens the
             * browser, where friends' public lobbies are listed too. */
            if (!ShSteam_Available())
            {
                snprintf(s_msg, sizeof(s_msg), "Steam is not running.");
            }
            else
            {
                snprintf(s_msg, sizeof(s_msg), "Accept the invite in Steam, or use Join Game in your friends list.");
                ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort(), 1);
                s_page = COOP_PAGE_ROOMS;
                s_sel  = 0;
            }
        }
        else { s_page = COOP_PAGE_ROOT; s_sel = 0; }
        break;

    case COOP_PAGE_ROOMS:
    {
        int rc = ShSession_RoomCount();
        if (s_sel < rc)
        {
            int                code  = 0;
            unsigned long long lobby = ShSession_RoomLobby(s_sel);
            ShSession_RoomGet(s_sel, &code, NULL, NULL, NULL, NULL, 0);
            if (lobby)
                ShSession_RequestJoin(lobby);
            else if (code > 0)
                ShSession_RequestJoinServer(Coop_ServerIp(), Coop_ServerPort(), code, 1);
            /* The guest boots when the host starts (title.c takes the world). */
        }
        else if (s_sel == rc) /* Refresh */
        {
            ShSession_RequestListServer(Coop_ServerIp(), Coop_ServerPort(), 1);
        }
        else { s_page = COOP_PAGE_JOIN; s_sel = 0; }
        break;
    }

    default:
        s_page = COOP_PAGE_ROOT;
        s_sel  = 0;
        break;
    }
}

void Pc_CoopMenu_SetHover(int row)
{
    s_hover = row;
    /* The highlight follows the pointer: a row under it is the selected row. */
    if (s_open && row >= 0 && row < Pc_CoopMenu_RowCount())
    {
        s_sel = row;
    }
}

/* Mouse hover drives the selection; a click selects then confirms. The renderer
 * (sh_net_ui.c) owns the row geometry, so it maps the pointer to a row and calls
 * these. */
void Pc_CoopMenu_SetSelected(int i)
{
    if (s_open && i >= 0 && i < Pc_CoopMenu_RowCount())
    {
        s_sel = i;
    }
}

void Pc_CoopMenu_Confirm(void)
{
    if (s_open)
    {
        Coop_Confirm();
    }
}

void Pc_CoopMenu_CancelPress(void)
{
    s_pressRow = -1;
}

/* Left/right on a setting row: the same change Enter makes, and the other
 * way for the rows that have one. 1 when the row took it. */
static int Coop_Adjust(int dir)
{
    switch (s_page)
    {
    case COOP_PAGE_HOST_SETUP:
        switch (s_sel)
        {
        case 0: if (!COOP_WEB) s_hostMode = (s_hostMode + 3 + dir) % 3; return 1;
        case 1: s_setMaxPlayers += dir; if (s_setMaxPlayers > 4) s_setMaxPlayers = 2; if (s_setMaxPlayers < 2) s_setMaxPlayers = 4; return 1;
        case 2: s_setPublic = !s_setPublic; return 1;
        case 3: s_setLiving = !s_setLiving; return 1;
        case 4: s_setFps = (s_setFps == 60) ? 30 : 60; return 1;
        case 5: Coop_StepSave(dir); return 1;
        default: return 0;
        }
    case COOP_PAGE_SPLIT_ONLINE:
        switch (s_sel)
        {
        case 0: s_setMaxPlayers += dir; if (s_setMaxPlayers > 4) s_setMaxPlayers = 2; if (s_setMaxPlayers < 2) s_setMaxPlayers = 4; return 1;
        case 1: s_setPublic = !s_setPublic; return 1;
        case 2: s_setLiving = !s_setLiving; return 1;
        case 3: s_setFps = (s_setFps == 60) ? 30 : 60; return 1;
        default: return 0;
        }
    case COOP_PAGE_SPLIT:
        switch (s_sel)
        {
        case 0: case 1: case 2: case 3: Coop_Confirm(); return 1; /* the same cycle Enter makes */
        case 4: Coop_StepSave(dir); return 1;
        case 5: s_splitOnline = (s_splitOnline + 3 + dir) % 3; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

void Pc_CoopMenu_Update(int cancel, int up, int down, int confirm, int left, int right)
{
    int rows;
    if (!s_open)
    {
        return;
    }
    /* While a text field is active the keyboard belongs to it (the overlay feeds
     * Pc_CoopMenu_FeedKeys); ignore navigation so typing never moves the row. */
    if (s_editKind != COOP_EDIT_NONE)
    {
        s_clickPrevHeld = Pc_MouseCursor_LeftHeld();
        s_pressRow      = -1;
        return;
    }
    /* The Enter or tap that saved a field also reaches the pad as a confirm a
     * frame later; taken as one, it opened the same field again. */
    if (SDL_GetTicks() - s_editEndMs < 250u)
    {
        s_clickPrevHeld = Pc_MouseCursor_LeftHeld();
        s_pressRow      = -1;
        return;
    }

    /* Room browser: keep the list live (player counts, new rooms) while it is
     * open. Coop_RefreshServerList is a no-op once we are in a room. */
    if (s_page == COOP_PAGE_ROOMS)
    {
        static unsigned int s_roomsNextMs = 0;
        unsigned int        now = SDL_GetTicks();
        if ((int)(now - s_roomsNextMs) >= 0)
        {
            Coop_RefreshServerList();
            s_roomsNextMs = now + 2000;
        }
    }
    /* A guest whose join just completed lands in the live lobby (status line
     * shows the real member count) instead of staring at a stale browser row. */
    if ((s_page == COOP_PAGE_ROOMS || s_page == COOP_PAGE_JOIN) &&
        ShSession_Active() && !ShSession_IsHost())
    {
        s_page = COOP_PAGE_HOST;
        s_sel  = 0;
    }
    rows = Pc_CoopMenu_RowCount();

    /* A click confirms the row the renderer said was under the pointer when
     * it went down, on the release: a finger that drags the list instead
     * (Pc_CoopMenu_CancelPress) picks nothing. The edges come from our OWN
     * previous-held state, so each click fires exactly once even if this runs
     * more than once per rendered frame (fixed-timestep catch-up) -- a single
     * press firing several confirms was cycling settings past themselves. */
    {
        int held     = Pc_MouseCursor_LeftHeld();
        int pressed  = held && !s_clickPrevHeld;
        int released = !held && s_clickPrevHeld;
        s_clickPrevHeld = held;
        if (pressed)
        {
            s_pressRow = (s_hover >= 0 && s_hover < rows) ? s_hover : -1;
        }
        if (released)
        {
            int row = s_pressRow;
            s_pressRow = -1;
            /* Released over another row: not a pick. A hidden pointer (-1) on
             * the release frame is a lifted finger, still the pressed row. */
            if (row >= 0 && row < rows && (s_hover < 0 || s_hover == row))
            {
                s_sel = row;
                Sd_PlaySfx(Sfx_MenuConfirm, 0, Q8(0.25f));
                Coop_Confirm();
                return;
            }
        }
    }

    if (up)
    {
        s_sel = (s_sel + rows - 1) % rows;
        Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
    }
    if (down)
    {
        s_sel = (s_sel + 1) % rows;
        Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
    }
    if ((left || right) && Coop_Adjust(right ? 1 : -1))
    {
        Sd_PlaySfx(Sfx_MenuMove, 0, Q8(0.25f));
        return;
    }
    if (confirm)
    {
        Sd_PlaySfx(Sfx_MenuConfirm, 0, Q8(0.25f));
        Coop_Confirm();
        return;
    }
    if (cancel)
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, Q8(0.25f));
        if (s_page == COOP_PAGE_PLAYER)
        {
            s_page = COOP_PAGE_PLAYERS;
            s_sel  = 0;
        }
        else if (s_page == COOP_PAGE_SAVES)
        {
            Coop_CloseSaves(); /* the pick stays as it was */
        }
        else if (s_page == COOP_PAGE_GAMEOVER)
        {
            /* A choice has to be made: there is no game to go back to. */
        }
        else if (s_page == COOP_PAGE_SPLIT_ONLINE)
        {
            s_page = COOP_PAGE_SPLIT;
            s_sel  = 6;
        }
        else if (s_page == COOP_PAGE_PLAYERS)
        {
            s_page = COOP_PAGE_INGAME; /* back out of the member list */
            s_sel  = Coop_PlayersRow();
        }
        else if (s_page == COOP_PAGE_ROOT || s_page == COOP_PAGE_INGAME)
        {
            Pc_CoopMenu_Close(); /* top level: close / resume */
        }
        else
        {
            s_page = s_inGame ? COOP_PAGE_INGAME : COOP_PAGE_ROOT;
            s_sel  = 0;
        }
    }
}
