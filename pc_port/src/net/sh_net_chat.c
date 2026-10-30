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
#include "pc_config.h"
#include "sh_log.h"

/* How long a line stays fully lit, then how long it fades, once nothing new
 * has arrived and the box is closed. While the box is open every recent line
 * is held at full. */
#define CHAT_SHOW_MS 9000
#define CHAT_FADE_MS 1500
#define CHAT_LOG_MAX 48       /* lines kept across every channel; the last few of the chosen one are shown */
#define CHAT_SHOWN   6        /* lines drawn when idle */
#define CHAT_SHOWN_OPEN 10    /* lines drawn while composing */
#define CHAT_SHOWN_TOUCH 5    /* lines drawn while typing on a touchscreen */
#define CHAT_SYSTEM  (-1)     /* a line of the game's own: shown on every channel */
#define CHAT_INPUT_MAX 450    /* typed bytes; sent as several lines of at most CHAT_PIECE_MAX */
#define CHAT_PIECE_MAX 150    /* one line on the wire: SHNET_CHAT_MAX is 160 with the NUL */

typedef struct
{
    char         text[SHNET_CHAT_MAX + SHNET_NAME_MAX + 4];
    int          scope;
    unsigned int ms;          /* arrival, for the fade */
} ChatLine;

static ChatLine s_log[CHAT_LOG_MAX];
static int      s_logCount;

static int  s_open;
static int  s_hintShown;                 /* "Press Tab..." once per run */
static int  s_scope = SHNET_CHAT_ALL;    /* the channel shown, and sent to (All sends to Global) */
static int  s_vis[CHAT_LOG_MAX];         /* indices into s_log on the chosen channel, this frame */
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

static void Chat_Push(int scope, const char* line)
{
    ChatLine* l;
    if (s_logCount == CHAT_LOG_MAX)
    {
        memmove(&s_log[0], &s_log[1], sizeof(s_log[0]) * (CHAT_LOG_MAX - 1));
        s_logCount--;
    }
    l = &s_log[s_logCount++];
    SDL_strlcpy(l->text, line, sizeof(l->text));
    l->scope = scope;
    l->ms    = SDL_GetTicks();
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
            SDL_strlcpy(out, m->name, (size_t)cap);
            return;
        }
    }
    SDL_strlcpy(out, "?", (size_t)cap);
}

void ShNetChat_CoopIn(unsigned long long from, const unsigned char* p, int len)
{
    char name[SHSESSION_NAME_MAX];
    char text[SHNET_CHAT_MAX];
    char line[sizeof(s_log[0].text)];
    int  n = len - 1;
    if (len < 2) return;
    if (n > SHNET_CHAT_MAX - 1) n = SHNET_CHAT_MAX - 1;
    memcpy(text, p + 1, (size_t)n);
    text[n] = '\0';
    text[strcspn(text, "\r\n")] = '\0';
    if (!text[0]) return;
    ShNetChat_Censor(text);
    Chat_MemberName(from, name, (int)sizeof(name));
    snprintf(line, sizeof(line), "%s: %s", name, text);
    Chat_Push(SHNET_CHAT_COOP, line);
}

void ShNetChat_Tick(void)
{
    int          scope;
    unsigned int fromId;
    char         name[SHNET_NAME_MAX];
    char         text[SHNET_CHAT_MAX];
    char         line[sizeof(s_log[0].text)];

    while (ShNet_PopChat(&scope, &fromId, name, sizeof(name), text, sizeof(text)))
    {
        ShNetChat_Censor(text); /* an older server relays them as typed */
        snprintf(line, sizeof(line), "%s: %s", name[0] ? name : "?", text);
        Chat_Push(scope == SHNET_CHAT_GAME ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL, line);
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
    s_inputLen  = 0;
    s_input[0]  = '\0';
    s_haveKeys  = 0; /* so the key that opened us is not read as a keystroke */
    if (!s_hintShown)
    {
        s_hintShown = 1;
        ShNetChat_AddSystem("Press Tab to switch channels");
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
    char          name[SHSESSION_NAME_MAX];
    char          line[sizeof(s_log[0].text)];
    int           n = (int)strlen(piece);
    if (n > SHNET_CHAT_MAX - 1) n = SHNET_CHAT_MAX - 1;
    b[0] = SHNET_CTRL_CHAT;
    memcpy(b + 1, piece, (size_t)n);
    b[1 + n] = '\0';
    ShSession_SendGame(SHNET_MSG_S_CTRL, b, 2 + n, 0, 1);
    Chat_MemberName(ShSession_SelfId(), name, (int)sizeof(name));
    if (name[0] == '?' && g_PcConfig.onlineName[0]) SDL_strlcpy(name, g_PcConfig.onlineName, sizeof(name));
    snprintf(line, sizeof(line), "%s: %s", name, piece);
    Chat_Push(SHNET_CHAT_COOP, line);
}

static void Chat_Submit(void)
{
    if (s_inputLen > 0)
    {
        const int scope = Chat_Scope();
        const int coop  = (scope == SHNET_CHAT_COOP);
        /* All shows everything but speaks to everyone: it sends as Global. */
        const int sendScope = (scope == SHNET_CHAT_GAME) ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL;
        ShNetChat_Censor(s_input);
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

static int Chat_OnChannel(const ChatLine* l)
{
    const int sc = Chat_Scope();
    return l->scope == CHAT_SYSTEM || sc == SHNET_CHAT_ALL || l->scope == sc;
}

/* The lines of the chosen channel to draw this frame: the newest few, minus
 * the ones that have faded out while the box is closed. */
static int Chat_Collect(void)
{
    /* Typing on a touchscreen, the box sits above the keyboard: fewer lines. */
    const int shown = s_open ? (s_touchText ? CHAT_SHOWN_TOUCH : CHAT_SHOWN_OPEN) : CHAT_SHOWN;
    int       i;
    s_visCount = 0;
    for (i = 0; i < s_logCount; i++)
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
