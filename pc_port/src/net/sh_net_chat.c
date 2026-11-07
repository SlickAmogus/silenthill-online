/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See sh_net_chat.h. Game-thread only. */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_chat.h"
#include "sh_net_chat_filter.h"
#include "sh_net_session.h"
#include "sh_net_social.h"
#include "pc_config.h"
#include "pc_mouse_cursor.h" /* the wheel reads back through the history */
#include "sh_log.h"

/* How long a line stays fully lit, then how long it fades, once nothing new
 * has arrived and the box is closed. While the box is open every recent line
 * is held at full. */
#define CHAT_SHOW_MS 9000
#define CHAT_FADE_MS 1500
#define CHAT_LOG_MAX 160      /* lines kept across every channel: the history scrolled while composing */
#define CHAT_SHOWN   6        /* lines drawn when idle */
#define CHAT_SHOWN_OPEN 10    /* lines drawn while composing */
#define CHAT_SHOWN_TOUCH 5    /* lines drawn while typing on a touchscreen */
#define CHAT_SYSTEM  (-1)     /* a line of the game's own: shown on every channel */
#define CHAT_PM      SHNETCHAT_PM /* a private message, either way: shown on every channel */
#define CHAT_INPUT_MAX 450    /* typed bytes; sent as several lines of at most CHAT_PIECE_MAX */
#define CHAT_PIECE_MAX 150    /* one line on the wire: SHNET_CHAT_MAX is 160 with the NUL */

typedef struct
{
    char         text[SHNET_CHAT_MAX + SHNET_NAME_MAX + 12];
    int          scope;
    unsigned int ms;          /* arrival, for the fade */
    int          nameLen;     /* the speaker's name at the start of text, drawn in their colour; 0 none */
    float        nr, ng, nb;
} ChatLine;

static ChatLine s_log[CHAT_LOG_MAX];
static int      s_logCount;

static int  s_open;
static int  s_hintShown;                 /* the chat hint (channels, /help): once per run, if the keys line has not said it */
static int  s_scope = SHNET_CHAT_ALL;    /* the channel shown, and sent to (All sends to Global) */
static int  s_vis[CHAT_LOG_MAX];         /* indices into s_log shown this frame */
static int  s_scroll;                    /* composing: lines scrolled back from the newest */
static int  s_visCount;
static char s_input[CHAT_INPUT_MAX];
static int  s_inputLen;

/* At the main menu (and the multiplayer lobby, which is drawn over it) there
 * is no level to talk to: only Global, and the room's own channel, called
 * Lobby there, while in one. */
static int Chat_InMenus(void)
{
    return g_GameWork.gameState == GameState_MainMenu;
}

/* The channel actually in use: s_scope, or Global where the menus have no
 * such channel. s_scope itself is kept for when the game starts. */
static int Chat_Scope(void)
{
    if (Chat_InMenus() && !(s_scope == SHNET_CHAT_COOP && ShSession_Active()))
    {
        return SHNET_CHAT_GLOBAL;
    }
    return s_scope;
}

static unsigned char s_prevKeys[SDL_NUM_SCANCODES];
static int           s_haveKeys;

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static ChatLine* Chat_Push(int scope, const char* line)
{
    ChatLine* l;
    if (s_logCount == CHAT_LOG_MAX)
    {
        memmove(&s_log[0], &s_log[1], sizeof(s_log[0]) * (CHAT_LOG_MAX - 1));
        s_logCount--;
    }
    l = &s_log[s_logCount++];
    SDL_strlcpy(l->text, line, sizeof(l->text));
    l->scope   = scope;
    l->ms      = SDL_GetTicks();
    l->nameLen = 0;
    if (scope != CHAT_SYSTEM)
    {
        SH_DBG("[CHAT] %s %s", ShNetChat_ScopeName(scope), line);
    }
    if (s_open && s_scroll > 0) s_scroll++; /* reading back: the view stays where it is */
    return l;
}

/* "name: text", the name in the speaker's colour. */
static void Chat_PushSaid(int scope, unsigned long long id, const char* name, const char* text)
{
    char      line[sizeof(s_log[0].text)];
    ChatLine* l;
    snprintf(line, sizeof(line), "%s: %s", name, text);
    l = Chat_Push(scope, line);
    l->nameLen = (int)strlen(name);
    if (l->nameLen > (int)strlen(l->text)) l->nameLen = (int)strlen(l->text);
    ShSocial_Color(id, &l->nr, &l->ng, &l->nb);
}

void ShNetChat_AddWhisper(const char* text)
{
    if (text && text[0])
    {
        Chat_Push(CHAT_PM, text);
    }
}

/* Players on this map called `name`, us included: a Level line names its
 * speaker with their #tag only when two of them share it here. */
static int Chat_SameNameHere(const char* name)
{
    int i, n = ShNet_GhostCount(), k = 0;
    if (g_PcConfig.onlineName[0] && !SDL_strcasecmp(g_PcConfig.onlineName, name)) k++;
    for (i = 0; i < n; i++)
    {
        const ShNetGhost* g = ShNet_Ghost(i);
        if (g && !SDL_strcasecmp(g->name, name)) k++;
    }
    return k;
}

/* The room's members called `name`: two of them, and Coop lines carry tags. */
static int Chat_SameNameInRoom(const char* name)
{
    int i, n = ShSession_Active() ? ShSession_MemberCount() : 0, k = 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && !SDL_strcasecmp(m->name, name)) k++;
    }
    return k;
}

void ShNetChat_AddSystem(const char* text)
{
    if (text && text[0])
    {
        Chat_Push(CHAT_SYSTEM, text);
    }
}

const char* ShNetChat_ScopeName(int scope)
{
    switch (scope)
    {
    case SHNET_CHAT_GLOBAL: return "Global";
    case SHNET_CHAT_GAME:   return "Level";
    case SHNET_CHAT_COOP:   return Chat_InMenus() ? "Lobby" : "Coop";
    case SHNET_CHAT_ALL:    return "All";
    case CHAT_PM:           return "Whisper";
    default:                return "";
    }
}

/* The name a room member goes by, or "?" when they are not (or no longer) in it. */
static void Chat_MemberName(unsigned long long id, char* out, int cap)
{
    int i, n = ShSession_Active() ? ShSession_MemberCount() : 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == id && m->name[0])
        {
            ShSocial_FormatName(m->name, (unsigned int)m->tag, Chat_SameNameInRoom(m->name) > 1, out, cap);
            return;
        }
    }
    SDL_strlcpy(out, "?", (size_t)cap);
}

void ShNetChat_CoopIn(unsigned long long from, const unsigned char* p, int len)
{
    char name[SHSESSION_NAME_MAX + 8];
    char text[SHNET_CHAT_MAX];
    int  n = len - 1;
    if (len < 2) return;
    if (n > SHNET_CHAT_MAX - 1) n = SHNET_CHAT_MAX - 1;
    memcpy(text, p + 1, (size_t)n);
    text[n] = '\0';
    text[strcspn(text, "\r\n")] = '\0';
    if (!text[0]) return;
    /* In someone else's game a player we blocked is still there, but what
     * they say is not. */
    if (ShSocial_BlockedId(from)) return;
    ShNetChat_Censor(text);
    Chat_MemberName(from, name, (int)sizeof(name));
    Chat_PushSaid(SHNET_CHAT_COOP, from, name, text);
    ShSocial_Speak(from, text);
}

/* The bound key's name for the controls line, or NULL when unbound. */
static const char* Chat_KeyName(const char* bound, const char* fallback)
{
    const char* k = (bound && bound[0]) ? bound : fallback;
    if (!k || !k[0] || SDL_strcasecmp(k, "NONE") == 0) return NULL;
    return k;
}

static void Chat_Part(char* out, int cap, const char* key, const char* what)
{
    size_t n = strlen(out);
    if (!key || (int)n >= cap - 1) return;
    snprintf(out + n, (size_t)(cap - (int)n), "%s%s %s", n ? ",  " : "", key, what);
}

/* The multiplayer controls, to this player alone, a moment after each load
 * into the living world and again when a co-op game starts: nothing else in
 * the game says which keys chat or open the menu. Shown as bound, or as the
 * touch buttons on a touchscreen. */
static void Chat_HelpTick(void)
{
    extern int          g_PcCoopGame;
    extern int          ShNet_WorldConnected(void);
    static int          s_shown;   /* 1 the living world's line, 2 co-op's */
    static unsigned int s_since;
    const int mode = g_PcCoopGame ? 2 : (ShNet_WorldConnected() ? 1 : 0);
    const int live = g_GameWork.gameState == GameState_InGame && !(g_SysWork.sysFlags & SysFlag_DemoActive) &&
                     g_SysWork.sysState == SysState_Gameplay;
    int       touch = 0;
    char      line[256];

    if (g_GameWork.gameState == GameState_MainMenu)
    {
        s_shown = 0;
        s_since = 0;
        return;
    }
    if (!live || mode <= s_shown)
    {
        s_since = 0;
        return;
    }
    if (!s_since)
    {
        s_since = SDL_GetTicks() | 1u;
        return;
    }
    if (SDL_GetTicks() - s_since < 2500u) return;
    s_shown = mode;
    s_since = 0;

#if defined(SH_IOS) || defined(__ANDROID__)
    touch = 1;
#elif defined(__EMSCRIPTEN__)
    {
        extern int Web_IsTouchDevice(void);
        touch = Web_IsTouchDevice();
    }
#endif
    if (touch)
    {
        snprintf(line, sizeof(line), "%s: the buttons at the top of the screen: the lines open chat, the two heads "
                 "the multiplayer menu%s.", mode == 2 ? "Co-op" : "Online",
                 mode == 2 ? ", the speech bubble voice lines" : " (leave a message, invite a player)");
        ShNetChat_AddSystem(line);
        return;
    }
    line[0] = '\0';
    Chat_Part(line, (int)sizeof(line), Chat_KeyName(g_PcConfig.keyChatGame, NULL), "chat");
    /* The channel key switches without typing; mid-line it would type its
     * letter, so Tab does it there. */
    {
        const char* cyc = Chat_KeyName(g_PcConfig.keyChatCycle, NULL);
        char        both[48];
        if (cyc) snprintf(both, sizeof(both), "%s or Tab", cyc);
        Chat_Part(line, (int)sizeof(line), cyc ? both : "Tab", "switch channel");
    }
    Chat_Part(line, (int)sizeof(line), Chat_KeyName(g_PcConfig.keyOnlineMemo, NULL),
              mode == 2 ? "multiplayer menu" : "multiplayer menu (leave a message, invite a player)");
    if (mode == 2) Chat_Part(line, (int)sizeof(line), Chat_KeyName(g_PcConfig.keyVocalize, "B"), "(hold) voice lines");
    Chat_Part(line, (int)sizeof(line), Chat_KeyName(g_PcConfig.keyOnlinePlayers, NULL), "who is online");
    if (!line[0]) return;
    {
        char full[320];
        snprintf(full, sizeof(full), "%s keys: %s.  Type /help in chat for commands.  Change keys under Controls.",
                 mode == 2 ? "Co-op" : "Online", line);
        ShNetChat_AddSystem(full);
        s_hintShown = 1;
    }
}

void ShNetChat_Tick(void)
{
    int          scope;
    unsigned int fromId, tag, rf, token;
    char         name[SHNET_NAME_MAX];
    char         who[SHNET_NAME_MAX + 8];
    char         text[SHNET_CHAT_MAX];

    Chat_HelpTick();
    ShSocial_Tick();
    while (ShNet_PopChatEx(&scope, &fromId, name, sizeof(name), text, sizeof(text), &tag, &rf, &token))
    {
        int showTag;
        if (ShSocial_BlockedToken(token)) continue;
        ShNetChat_Censor(text); /* an older server relays them as typed */
        /* The #tag only where the name alone is not enough: on Global, when
         * anyone online shares it; on Level, when someone on this map does. */
        showTag = (scope == SHNET_CHAT_GAME) ? Chat_SameNameHere(name) > 1 : (rf & SHNET_RF_DUPNAME) != 0;
        ShSocial_FormatName(name[0] ? name : "?", tag, showTag, who, (int)sizeof(who));
        Chat_PushSaid(scope == SHNET_CHAT_GAME ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL, fromId, who, text);
        if (fromId != ShNet_SelfId()) ShSocial_Speak(fromId, text);
    }
}

/* ------------------------------------------------------------------ */
/* Compose                                                             */
/* ------------------------------------------------------------------ */

/* Touch typing: the chat button on a touchscreen. A phone keyboard's key
 * presses come and go between two frames, so the key-state polling below
 * misses them; this path takes SDL's text events instead (on Android they
 * arrive on the Java thread, hence the lock). The browser has no keyboard to
 * raise for a canvas: there a hidden text box takes the typing (web_platform.c)
 * and the SDL events are ignored, or a desktop key would arrive twice. */
static int          s_touchText;
static SDL_SpinLock s_tLock;
static char         s_tTyped[128];
static int          s_tLen, s_tBack, s_tEnter, s_tEsc;
static int          s_tWatching;
#if defined(__EMSCRIPTEN__)
extern void Web_TextBegin(void);
extern void Web_TextEnd(void);
extern int  Web_TextTake(char* out, int cap);
#endif

static int SDLCALL Chat_TextWatch(void* user, SDL_Event* e)
{
    (void)user;
#if defined(__EMSCRIPTEN__)
    (void)e;
    return 0;
#else
    if (!s_touchText)
    {
        return 0;
    }
    SDL_AtomicLock(&s_tLock);
    if (e->type == SDL_TEXTINPUT)
    {
        const char* t = e->text.text;
        for (; *t && s_tLen < (int)sizeof(s_tTyped); t++)
        {
            s_tTyped[s_tLen++] = *t;
        }
    }
    else if (e->type == SDL_KEYDOWN)
    {
        const SDL_Scancode sc = e->key.keysym.scancode;
        if (sc == SDL_SCANCODE_BACKSPACE) s_tBack++;
        else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) s_tEnter = 1;
        else if (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_AC_BACK) s_tEsc = 1;
    }
    SDL_AtomicUnlock(&s_tLock);
    return 0;
#endif
}

void ShNetChat_Open(void)
{
    extern int Pc_QuickOptions_IsOpen(void);
    /* Not under the quick menu, which owns the input while it is up. */
    if (Pc_QuickOptions_IsOpen())
    {
        return;
    }
    s_open      = 1;
    s_scroll    = 0;
    s_inputLen  = 0;
    s_input[0]  = '\0';
    s_haveKeys  = 0; /* so the key that opened us is not read as a keystroke */
    if (!s_hintShown)
    {
        s_hintShown = 1;
        ShNetChat_AddSystem("Tab switches channels.  Type /help for chat commands.");
    }
}

/* Global -> Level -> Coop (while in a co-op game) -> All -> Global. At the
 * menus: Global -> Lobby (in a room) -> Global. */
static int Chat_NextScope(int sc)
{
    if (Chat_InMenus())
    {
        return (sc != SHNET_CHAT_COOP && ShSession_Active()) ? SHNET_CHAT_COOP : SHNET_CHAT_GLOBAL;
    }
    for (;;)
    {
        sc = (sc + 1) % 4;
        if (sc == SHNET_CHAT_COOP && !ShSession_Active()) continue;
        return sc;
    }
}

void ShNetChat_CycleScope(void)
{
    s_scope = Chat_NextScope(Chat_Scope());
    if (!s_open)
    {
        ShNetChat_Open();
    }
}

void ShNetChat_Close(void)
{
    s_open     = 0;
    s_inputLen = 0;
    s_input[0] = '\0';
    if (s_touchText)
    {
        s_touchText = 0;
        if (SDL_HasScreenKeyboardSupport())
        {
            SDL_StopTextInput();
        }
#if defined(__EMSCRIPTEN__)
        Web_TextEnd();
#endif
    }
}

void ShNetChat_OpenTouch(void)
{
    ShNetChat_Open();
    if (!s_open)
    {
        return;
    }
    SDL_AtomicLock(&s_tLock);
    s_tLen = s_tBack = s_tEnter = s_tEsc = 0;
    SDL_AtomicUnlock(&s_tLock);
    if (!s_tWatching)
    {
        SDL_AddEventWatch(Chat_TextWatch, NULL);
        s_tWatching = 1;
    }
    s_touchText = 1;
    /* On a phone, starting text input is what raises the keyboard; SDL can
     * count it as already on after the keyboard was dismissed, so always. */
    if (SDL_HasScreenKeyboardSupport())
    {
        SDL_StartTextInput();
    }
#if defined(__EMSCRIPTEN__)
    Web_TextBegin();
#endif
}

int ShNetChat_TouchTyping(void)
{
    return s_open && s_touchText;
}

int ShNetChat_Available(void)
{
    return (ShNet_Enabled() && ShNet_Status() == SHNET_ST_CONNECTED) || ShSession_Active();
}

static void Chat_Submit(void);
static void Chat_Append(char c);

/* What the touch path collected since the last frame. Only printable ASCII is
 * kept, as the server keeps only that. */
static void Chat_FeedTouch(void)
{
    char typed[160];
    int  n, back, enter, esc, i;

    SDL_AtomicLock(&s_tLock);
    n = s_tLen;
    memcpy(typed, s_tTyped, (size_t)n);
    back  = s_tBack;
    enter = s_tEnter;
    esc   = s_tEsc;
    s_tLen = s_tBack = s_tEnter = s_tEsc = 0;
    SDL_AtomicUnlock(&s_tLock);
#if defined(__EMSCRIPTEN__)
    {
        /* The text box reports a backspace as '\b' and Enter as '\n'. */
        char w[128];
        int  k, m = Web_TextTake(w, (int)sizeof(w));
        for (k = 0; k < m; k++)
        {
            if (w[k] == 27) esc = 1;
            else if (w[k] == '\b') back++;
            else if (w[k] == '\n') enter = 1;
            else if (n < (int)sizeof(typed)) typed[n++] = w[k];
        }
    }
#endif
    if (esc)
    {
        ShNetChat_Close();
        return;
    }
    while (back-- > 0 && s_inputLen > 0)
    {
        s_input[--s_inputLen] = '\0';
    }
    for (i = 0; i < n; i++)
    {
        if ((unsigned char)typed[i] >= 32 && (unsigned char)typed[i] <= 126)
        {
            Chat_Append(typed[i]);
        }
    }
    if (enter)
    {
        Chat_Submit();
    }
}

int ShNetChat_IsOpen(void)
{
    return s_open;
}

/* Hidden puts the feed away; the box still opens on its key, and the lines
 * show while it is open, so a hidden chat is still one you can take part in. */
void ShNetChat_SetHidden(int hidden)
{
    g_PcConfig.onlineChatHidden = hidden ? 1 : 0;
    PcConfig_SaveKeyValue("online_chat_hidden", hidden ? "1" : "0");
    if (hidden)
    {
        ShNetChat_Close();
    }
}

int ShNetChat_Hidden(void)
{
    return g_PcConfig.onlineChatHidden;
}

/* A Coop line goes to the room itself, as a control note, so a Steam or
 * Join-by-IP game has it too and the server never sees it. The room does not
 * echo, so the line is shown here as sent. */
static void Chat_SendCoop(const char* piece)
{
    unsigned char b[1 + SHNET_CHAT_MAX];
    char          name[SHSESSION_NAME_MAX + 8];
    int           n = (int)strlen(piece);
    if (n > SHNET_CHAT_MAX - 1) n = SHNET_CHAT_MAX - 1;
    b[0] = SHNET_CTRL_CHAT;
    memcpy(b + 1, piece, (size_t)n);
    b[1 + n] = '\0';
    ShSession_SendGame(SHNET_MSG_S_CTRL, b, 2 + n, 0, 1);
    Chat_MemberName(ShSession_SelfId(), name, (int)sizeof(name));
    if (name[0] == '?' && g_PcConfig.onlineName[0]) SDL_strlcpy(name, g_PcConfig.onlineName, sizeof(name));
    Chat_PushSaid(SHNET_CHAT_COOP, ShSession_SelfId(), name, piece);
}

static void Chat_Submit(void)
{
    if (s_inputLen > 0)
    {
        const int scope = Chat_Scope();
        const int coop  = (scope == SHNET_CHAT_COOP);
        /* All shows everything but speaks to everyone: it sends as Global. */
        const int sendScope = (scope == SHNET_CHAT_GAME) ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL;
        int       wait;
        ShNetChat_Censor(s_input);
        if (s_input[0] == '/')
        {
            char line[CHAT_INPUT_MAX];
            SDL_strlcpy(line, s_input, sizeof(line));
            ShNetChat_Close();
            SH_DBG("[CHAT] command %s", line[1] == 'w' || !SDL_strncasecmp(line, "/msg", 4) ? "(whisper)" : line);
            ShSocial_Command(line);
            return;
        }
        if (!coop && sendScope == SHNET_CHAT_GLOBAL && ShNet_Status() == SHNET_ST_CONNECTED &&
            (wait = ShSocial_GlobalWaitSec()) > 0)
        {
            /* The line stays in the box, to send when the wait is over. */
            char note[96];
            snprintf(note, sizeof(note), "[Global chat: one message every 10 seconds; wait %d s]", wait);
            ShNetChat_AddSystem(note);
            return;
        }
        if (coop && !ShSession_Active())
        {
            ShNetChat_AddSystem("[not in a co-op game]");
        }
        else if (coop || ShNet_Status() == SHNET_ST_CONNECTED)
        {
            /* No local echo on the server channels: the server sends the line
             * back to everyone including us, so it lands in the same list, in
             * order. A long message goes as several lines, broken at a space. */
            const char* p = s_input;
            while (*p)
            {
                char piece[CHAT_PIECE_MAX + 1];
                int  n;
                while (*p == ' ') p++;
                n = (int)strlen(p);
                if (n <= 0) break;
                if (n > CHAT_PIECE_MAX)
                {
                    int k = CHAT_PIECE_MAX;
                    while (k > CHAT_PIECE_MAX / 2 && p[k] != ' ') k--;
                    n = (p[k] == ' ') ? k : CHAT_PIECE_MAX;
                }
                memcpy(piece, p, (size_t)n);
                piece[n] = '\0';
                if (coop) Chat_SendCoop(piece);
                else      ShNet_SendChat(sendScope, piece);
                p += n;
            }
            if (!coop && sendScope == SHNET_CHAT_GLOBAL) ShSocial_GlobalSent();
        }
        else
        {
            ShNetChat_AddSystem("[not connected to a server]");
        }
    }
    ShNetChat_Close();
}

/* ------------------------------------------------------------------ */
/* Keyboard -> text                                                    */
/* ------------------------------------------------------------------ */

/* SDL scancode -> the glyph it types, lower and shifted. Only the printable
 * keys a chat line needs; everything else is ignored. Digits and the symbol
 * row are spelled out because SDL's scancode order is US-keyboard positional,
 * not ASCII. */
static char Chat_Glyph(int sc, int shift)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        char base = (char)('a' + (sc - SDL_SCANCODE_A));
        return shift ? (char)(base - 32) : base;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
    {
        static const char lo[] = "123456789";
        static const char hi[] = "!@#$%^&*(";
        return shift ? hi[sc - SDL_SCANCODE_1] : lo[sc - SDL_SCANCODE_1];
    }
    switch (sc)
    {
    case SDL_SCANCODE_0:            return shift ? ')' : '0';
    case SDL_SCANCODE_SPACE:        return ' ';
    case SDL_SCANCODE_MINUS:        return shift ? '_' : '-';
    case SDL_SCANCODE_EQUALS:       return shift ? '+' : '=';
    case SDL_SCANCODE_LEFTBRACKET:  return shift ? '{' : '[';
    case SDL_SCANCODE_RIGHTBRACKET: return shift ? '}' : ']';
    case SDL_SCANCODE_BACKSLASH:    return shift ? '|' : '\\';
    case SDL_SCANCODE_SEMICOLON:    return shift ? ':' : ';';
    case SDL_SCANCODE_APOSTROPHE:   return shift ? '"' : '\'';
    case SDL_SCANCODE_GRAVE:        return shift ? '~' : '`';
    case SDL_SCANCODE_COMMA:        return shift ? '<' : ',';
    case SDL_SCANCODE_PERIOD:       return shift ? '>' : '.';
    case SDL_SCANCODE_SLASH:        return shift ? '?' : '/';
    default:                        return '\0';
    }
}

static void Chat_Append(char c)
{
    if (c && s_inputLen < CHAT_INPUT_MAX - 1)
    {
        s_input[s_inputLen++] = c;
        s_input[s_inputLen]   = '\0';
    }
}

void ShNetChat_FeedKeys(const unsigned char* ks)
{
    int sc;
    int shift;
    int ctrl;

    if (!s_open || !ks)
    {
        return;
    }
    if (s_touchText)
    {
        Chat_FeedTouch();
        return;
    }
    /* First frame after opening: seed prev from live so the opening key press
     * (Y / U) is not itself typed, and nothing held from gameplay leaks in. */
    if (!s_haveKeys)
    {
        memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
        s_haveKeys = 1;
        return;
    }

    shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    ctrl  = ks[SDL_SCANCODE_LCTRL]  || ks[SDL_SCANCODE_RCTRL];

    if (ks[SDL_SCANCODE_ESCAPE] && !s_prevKeys[SDL_SCANCODE_ESCAPE])
    {
        ShNetChat_Close();
        goto done;
    }
    if ((ks[SDL_SCANCODE_RETURN] && !s_prevKeys[SDL_SCANCODE_RETURN]) ||
        (ks[SDL_SCANCODE_KP_ENTER] && !s_prevKeys[SDL_SCANCODE_KP_ENTER]))
    {
        Chat_Submit();
        goto done;
    }
    /* Tab always cycles the channel; U cycles too, but only while nothing has
     * been typed yet, so it stays a normal letter once you are writing. */
    if (ks[SDL_SCANCODE_TAB] && !s_prevKeys[SDL_SCANCODE_TAB])
    {
        s_scope = Chat_NextScope(s_scope);
    }
    else if (s_inputLen == 0 && ks[SDL_SCANCODE_U] && !s_prevKeys[SDL_SCANCODE_U])
    {
        s_scope = Chat_NextScope(s_scope);
        goto done; /* consume it: don't also type 'u' */
    }

    /* Page Up / Page Down and the wheel read back through the history. */
    {
        int step = Pc_MouseCursor_WheelNotches() * 3;
        if (ks[SDL_SCANCODE_PAGEUP] && !s_prevKeys[SDL_SCANCODE_PAGEUP])     step += CHAT_SHOWN_OPEN - 1;
        if (ks[SDL_SCANCODE_PAGEDOWN] && !s_prevKeys[SDL_SCANCODE_PAGEDOWN]) step -= CHAT_SHOWN_OPEN - 1;
        s_scroll += step;
        if (s_scroll > s_logCount - 1) s_scroll = s_logCount - 1;
        if (s_scroll < 0) s_scroll = 0;
    }

    if (ks[SDL_SCANCODE_BACKSPACE] && !s_prevKeys[SDL_SCANCODE_BACKSPACE] && s_inputLen > 0)
    {
        s_input[--s_inputLen] = '\0';
    }

    if (!ctrl)
    {
        for (sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        {
            if (ks[sc] && !s_prevKeys[sc])
            {
                Chat_Append(Chat_Glyph(sc, shift));
            }
        }
    }

done:
    memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
    if (!s_open)
    {
        s_haveKeys = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Draw-facing                                                         */
/* ------------------------------------------------------------------ */

static float Chat_Alpha(const ChatLine* l)
{
    unsigned int age;
    if (s_open)
    {
        return 1.0f; /* everything held up while composing */
    }
    age = SDL_GetTicks() - l->ms;
    if (age <= CHAT_SHOW_MS)
    {
        return 1.0f;
    }
    if (age >= CHAT_SHOW_MS + CHAT_FADE_MS)
    {
        return 0.0f;
    }
    return 1.0f - (float)(age - CHAT_SHOW_MS) / (float)CHAT_FADE_MS;
}

/* The channel chosen is where a line is SENT. Every line is shown, whatever
 * its channel (tagged with it when it is not the one chosen): a Level line
 * that arrived while Coop was chosen was never drawn, nor in the history,
 * and showed only for the moment Tab passed through All. */
static int Chat_OnChannel(const ChatLine* l)
{
    (void)l;
    return 1;
}

/* The lines to draw this frame: the newest few (scrolled back while
 * composing), minus the ones that have faded out while the box is closed. */
static int Chat_Collect(void)
{
    /* Typing on a touchscreen, the box sits above the keyboard: fewer lines. */
    const int shown = s_open ? (s_touchText ? CHAT_SHOWN_TOUCH : CHAT_SHOWN_OPEN) : CHAT_SHOWN;
    const int last  = s_logCount - (s_open ? s_scroll : 0);
    int       i;
    s_visCount = 0;
    for (i = 0; i < last; i++)
    {
        if (!Chat_OnChannel(&s_log[i])) continue;
        if (!s_open && Chat_Alpha(&s_log[i]) <= 0.0f) continue;
        if (s_visCount == shown)
        {
            memmove(&s_vis[0], &s_vis[1], sizeof(s_vis[0]) * (size_t)(shown - 1));
            s_visCount--;
        }
        s_vis[s_visCount++] = i;
    }
    return s_visCount;
}

int ShNetChat_DisplayActive(void)
{
    if ((g_PcConfig.onlineChatHidden && !s_open) || !ShNet_Enabled())
    {
        return 0;
    }
    if (s_open)
    {
        return 1;
    }
    return Chat_Collect() > 0;
}

int ShNetChat_LineCount(void)
{
    if (g_PcConfig.onlineChatHidden && !s_open)
    {
        return 0;
    }
    return Chat_Collect();
}

static const ChatLine* Chat_At(int i)
{
    return (i >= 0 && i < s_visCount) ? &s_log[s_vis[i]] : NULL;
}

const char* ShNetChat_LineText(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? l->text : "";
}

int ShNetChat_LineScope(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? l->scope : CHAT_SYSTEM;
}

float ShNetChat_LineAlpha(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? Chat_Alpha(l) : 0.0f;
}

int ShNetChat_LineName(int i, float* r, float* g, float* b)
{
    const ChatLine* l = Chat_At(i);
    if (!l || l->nameLen <= 0) return 0;
    *r = l->nr; *g = l->ng; *b = l->nb;
    return l->nameLen;
}

int ShNetChat_Composing(void)
{
    return s_open;
}

int ShNetChat_Scope(void)
{
    return Chat_Scope();
}

const char* ShNetChat_ComposeText(void)
{
    return s_input;
}
