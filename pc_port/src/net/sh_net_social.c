/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sh_net_social.c - chat commands, tags, blocks, colours, speech (see the header). */

#include "sh_net_social.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_config.h"
#include "pc_coop_menu.h"
#include "sh_net.h"
#include "sh_net_chat.h"
#include "sh_net_invite.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_log.h"

#define SOC_LOOKUP_MS   5000u  /* a lookup the server never answered */
#define SOC_PEND_MAX    8
#define SOC_SPEECH_MAX  16
#define SOC_SPEECH_MS   5000u
#define SOC_SPEECH_FADE 1000u
#define SOC_COLOR_MAX   32

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

/* Light, saturated enough to tell apart, and every one readable over the
 * fog's grey and over a dark interior. */
static const struct { const char* name; float r, g, b; } kColors[] = {
    { "red",     1.00f, 0.52f, 0.50f },
    { "orange",  1.00f, 0.70f, 0.40f },
    { "yellow",  1.00f, 0.92f, 0.45f },
    { "lime",    0.75f, 1.00f, 0.45f },
    { "green",   0.48f, 0.95f, 0.55f },
    { "teal",    0.40f, 0.95f, 0.82f },
    { "cyan",    0.50f, 0.92f, 1.00f },
    { "sky",     0.55f, 0.78f, 1.00f },
    { "blue",    0.62f, 0.66f, 1.00f },
    { "purple",  0.80f, 0.62f, 1.00f },
    { "magenta", 1.00f, 0.55f, 0.95f },
    { "pink",    1.00f, 0.66f, 0.78f },
    { "gold",    0.95f, 0.80f, 0.35f },
    { "peach",   1.00f, 0.80f, 0.66f },
    { "mint",    0.70f, 1.00f, 0.80f },
    { "lavender",0.86f, 0.80f, 1.00f },
    { "white",   0.95f, 0.95f, 0.95f },
    { "silver",  0.78f, 0.80f, 0.84f },
};
#define SOC_RANDOM_COLORS 16 /* the random pick stays off white and silver */

static unsigned int s_colorSeed;
static struct { unsigned long long id; float r, g, b; } s_colorSet[SOC_COLOR_MAX];
static int s_colorSetN;

static int Soc_ParseColor(const char* s, float* r, float* g, float* b)
{
    size_t i;
    if (!s || !s[0]) return 0;
    if (s[0] == '#' && strlen(s) == 7)
    {
        unsigned int v = (unsigned int)strtoul(s + 1, NULL, 16);
        *r = (float)((v >> 16) & 0xFF) / 255.0f;
        *g = (float)((v >> 8) & 0xFF) / 255.0f;
        *b = (float)(v & 0xFF) / 255.0f;
        return 1;
    }
    for (i = 0; i < sizeof(kColors) / sizeof(kColors[0]); i++)
    {
        if (SDL_strcasecmp(s, kColors[i].name) == 0)
        {
            *r = kColors[i].r; *g = kColors[i].g; *b = kColors[i].b;
            return 1;
        }
    }
    return 0;
}

static unsigned long long Soc_SelfId(void)
{
    if (ShSession_Active() && ShSession_SelfId()) return ShSession_SelfId();
    return ShNet_WorldSelfId();
}

static int Soc_IsSelf(unsigned long long id)
{
    return id != 0 && (id == ShNet_WorldSelfId() || id == ShNet_SelfId() ||
                       (ShSession_Active() && id == ShSession_SelfId()));
}

void ShSocial_Color(unsigned long long id, float* r, float* g, float* b)
{
    unsigned int h;
    int          i;
    if (Soc_IsSelf(id) && Soc_ParseColor(g_PcConfig.onlineColor, r, g, b)) return;
    for (i = 0; i < s_colorSetN; i++)
    {
        if (s_colorSet[i].id == id)
        {
            *r = s_colorSet[i].r; *g = s_colorSet[i].g; *b = s_colorSet[i].b;
            return;
        }
    }
    /* Random per run, steady per player: the seed is picked once. */
    if (!s_colorSeed) s_colorSeed = (SDL_GetTicks() * 2654435761u) | 1u;
    h = (unsigned int)(id ^ (id >> 32)) * 2246822519u ^ s_colorSeed;
    h ^= h >> 15;
    h *= 3266489917u;
    h ^= h >> 13;
    i = (int)(h % SOC_RANDOM_COLORS);
    *r = kColors[i].r; *g = kColors[i].g; *b = kColors[i].b;
}

static void Soc_SetColor(unsigned long long id, float r, float g, float b)
{
    int i;
    for (i = 0; i < s_colorSetN && s_colorSet[i].id != id; i++) {}
    if (i == s_colorSetN)
    {
        if (s_colorSetN < SOC_COLOR_MAX) s_colorSetN++;
        else i = SOC_COLOR_MAX - 1;
    }
    s_colorSet[i].id = id;
    s_colorSet[i].r = r; s_colorSet[i].g = g; s_colorSet[i].b = b;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

void ShSocial_FormatName(const char* name, unsigned int tag, int showTag, char* out, int cap)
{
    if (showTag && tag) snprintf(out, (size_t)cap, "%s#%04u", name && name[0] ? name : "?", tag);
    else                snprintf(out, (size_t)cap, "%s", name && name[0] ? name : "?");
}

/* ------------------------------------------------------------------ */
/* Blocks                                                              */
/* ------------------------------------------------------------------ */

static unsigned int s_blocksPushedMs;
static int          s_blocksPushed;

static void Soc_PushBlocks(void)
{
    unsigned int toks[PC_ONLINE_BLOCK_MAX];
    int          i, n = 0;
    for (i = 0; i < PC_ONLINE_BLOCK_MAX; i++)
    {
        if (g_PcConfig.onlineBlocks[i].token) toks[n++] = g_PcConfig.onlineBlocks[i].token;
    }
    ShSession_RequestBlocks(toks, n);
    s_blocksPushed   = 1;
    s_blocksPushedMs = SDL_GetTicks();
}

int ShSocial_BlockedToken(unsigned int token)
{
    int i;
    if (!token) return 0;
    for (i = 0; i < PC_ONLINE_BLOCK_MAX; i++)
    {
        if (g_PcConfig.onlineBlocks[i].token == token) return 1;
    }
    return 0;
}

unsigned int ShSocial_TokenOf(unsigned long long id)
{
    int i, n;
    if (!id) return 0;
    n = ShNet_PeerCount();
    for (i = 0; i < n; i++)
    {
        const ShNetPeer* p = ShNet_Peer(i);
        if (p && p->playerId == (unsigned int)id) return p->token;
    }
    n = ShSession_Active() ? ShSession_MemberCount() : 0;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == id) return m->token;
    }
    return 0;
}

int ShSocial_BlockedId(unsigned long long id)
{
    return !Soc_IsSelf(id) && ShSocial_BlockedToken(ShSocial_TokenOf(id));
}

static void Soc_SaveBlock(int slot)
{
    char key[32], val[48];
    snprintf(key, sizeof(key), "online_block_%d", slot);
    if (g_PcConfig.onlineBlocks[slot].token)
        snprintf(val, sizeof(val), "%08x %s", g_PcConfig.onlineBlocks[slot].token, g_PcConfig.onlineBlocks[slot].name);
    else
        val[0] = '\0';
    PcConfig_SaveKeyValue(key, val);
}

/* Anyone in a room we host who comes from that address leaves it now. */
static void Soc_KickBlocked(unsigned int token)
{
    int i, n = ShSession_Active() ? ShSession_MemberCount() : 0;
    if (!ShSession_Active() || !ShSession_IsHost()) return;
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId != ShSession_SelfId() && m->token == token)
        {
            SH_DBG("[SOCIAL] blocked %s is in our game: removing them", m->name);
            ShSession_RequestKick(m->steamId);
        }
    }
}

static void Soc_Block(unsigned int token, const char* name)
{
    int i, slot = -1;
    char line[128];
    if (!token) return;
    if (ShSocial_BlockedToken(token))
    {
        snprintf(line, sizeof(line), "%s is already blocked.", name);
        ShNetChat_AddSystem(line);
        Soc_KickBlocked(token);
        return;
    }
    for (i = 0; i < PC_ONLINE_BLOCK_MAX && slot < 0; i++)
    {
        if (!g_PcConfig.onlineBlocks[i].token) slot = i;
    }
    if (slot < 0)
    {
        ShNetChat_AddSystem("Your block list is full (64). /unblock someone first.");
        return;
    }
    g_PcConfig.onlineBlocks[slot].token = token;
    snprintf(g_PcConfig.onlineBlocks[slot].name, sizeof(g_PcConfig.onlineBlocks[slot].name), "%s", name);
    Soc_SaveBlock(slot);
    Soc_PushBlocks();
    Soc_KickBlocked(token);
    SH_DBG("[SOCIAL] blocked %s (%08x)", name, token);
    snprintf(line, sizeof(line), "Blocked %s. You won't see them or their messages, and they can't join your games.", name);
    ShNetChat_AddSystem(line);
}

/* ------------------------------------------------------------------ */
/* Speech over heads                                                   */
/* ------------------------------------------------------------------ */

static struct { unsigned long long id; unsigned int ms; char text[SHNET_CHAT_MAX]; } s_speech[SOC_SPEECH_MAX];

void ShSocial_Speak(unsigned long long id, const char* text)
{
    int i, slot = 0;
    unsigned int oldest = 0xFFFFFFFFu;
    if (!id || !text || !text[0] || Soc_IsSelf(id)) return;
    for (i = 0; i < SOC_SPEECH_MAX; i++)
    {
        if (s_speech[i].id == id) { slot = i; break; }
        if (s_speech[i].ms < oldest) { oldest = s_speech[i].ms; slot = i; }
    }
    s_speech[slot].id = id;
    s_speech[slot].ms = SDL_GetTicks();
    snprintf(s_speech[slot].text, sizeof(s_speech[slot].text), "%s", text);
}

const char* ShSocial_Speech(unsigned long long id, float* alpha)
{
    int i;
    const unsigned int now = SDL_GetTicks();
    for (i = 0; i < SOC_SPEECH_MAX; i++)
    {
        unsigned int age;
        if (s_speech[i].id != id || !s_speech[i].text[0]) continue;
        age = now - s_speech[i].ms;
        if (age >= SOC_SPEECH_MS + SOC_SPEECH_FADE) return NULL;
        if (alpha) *alpha = (age <= SOC_SPEECH_MS) ? 1.0f : 1.0f - (float)(age - SOC_SPEECH_MS) / (float)SOC_SPEECH_FADE;
        return s_speech[i].text;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Global chat pace                                                    */
/* ------------------------------------------------------------------ */

static unsigned int s_globalMs;
static int          s_globalSent;

int ShSocial_GlobalWaitSec(void)
{
    unsigned int age;
    if (!s_globalSent) return 0;
    age = SDL_GetTicks() - s_globalMs;
    if (age >= SHNET_CHAT_GLOBAL_GAP_MS) return 0;
    return (int)((SHNET_CHAT_GLOBAL_GAP_MS - age + 999u) / 1000u);
}

void ShSocial_GlobalSent(void)
{
    s_globalMs   = SDL_GetTicks();
    s_globalSent = 1;
}

/* ------------------------------------------------------------------ */
/* Whispers                                                            */
/* ------------------------------------------------------------------ */

void ShSocial_ShowWhisper(int dir, const char* name, unsigned int tag, int showTag, const char* text)
{
    char who[SHNET_NAME_MAX + 8];
    char line[SHNET_CHAT_MAX + 48];
    ShSocial_FormatName(name, tag, showTag, who, (int)sizeof(who));
    if (dir == SHNET_PM_GONE) snprintf(line, sizeof(line), "Not delivered (they left): %s", text);
    else                      snprintf(line, sizeof(line), "%s %s: %s", dir == SHNET_PM_IN ? "From" : "To", who, text);
    ShNetChat_AddWhisper(line);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

enum { CMD_INVITE = 1, CMD_WHISPER, CMD_BLOCK, CMD_REPORT, CMD_COLOR };

static struct
{
    int          used, req, kind;
    unsigned int ms;
    char         query[SHNET_QUERY_MAX];
    char         arg[SHNET_CHAT_MAX * 3];
} s_pend[SOC_PEND_MAX];
static int s_nextReq = 1;

static const struct { const char* name; const char* usage; const char* help; } kCmds[] = {
    { "invite",    "/invite <name>",            "Invites a player into your game. If you are not in one yet, you can host from where you are or go to the lobby first." },
    { "whisper",   "/whisper <name> <message>", "Sends a private message only that player sees. /w and /msg work too." },
    { "join",      "/join <code>",              "Joins the room with that 4-digit code." },
    { "kick",      "/kick <name>",              "Removes a player from the game or lobby you are hosting." },
    { "block",     "/block <name>",             "Hides a player from you: their character, chat and messages to you. They can't join games you host, and are removed if they are in one. They are not told." },
    { "blocklist", "/blocklist",                "Lists the players you have blocked." },
    { "unblock",   "/unblock <name or number>", "Lifts a block. The number is the one /blocklist shows." },
    { "report",    "/report <name> \"reason\"", "Reports a player to the server admin, with your private messages with them." },
    { "color",     "/color [name] <color>",     "Changes the colour a name shows in on your screen: red, orange, yellow, lime, green, teal, cyan, sky, blue, purple, magenta, pink, gold, peach, mint, lavender, white, silver, or #RRGGBB. Only you see it." },
    { "accept",    "/accept",                   "Accepts the invite waiting for you." },
    { "decline",   "/decline",                  "Declines the invite waiting for you." },
    { "help",      "/help [command]",           "Lists the commands, or explains one." },
};

static void Soc_Say(const char* fmt, ...)
{
    char    line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ShNetChat_AddSystem(line);
}

static const char* Soc_SkipSpace(const char* s)
{
    while (*s == ' ') s++;
    return s;
}

/* Names may hold spaces ("Harry Mason"), so the name a command starts with is
 * the longest name we know of that fits; a quoted name is taken as written;
 * otherwise the first word. A "#1234" right after it belongs to it. */
static int Soc_KnownNameAt(const char* s, const char* name)
{
    size_t n = strlen(name);
    if (!n || SDL_strncasecmp(s, name, n) != 0) return 0;
    return (s[n] == '\0' || s[n] == ' ' || s[n] == '#') ? (int)n : 0;
}

static const char* Soc_TakeName(const char* s, char* out, int cap)
{
    int best = 0, i, n;
    s = Soc_SkipSpace(s);
    out[0] = '\0';
    if (*s == '"')
    {
        const char* e = strchr(s + 1, '"');
        int         len = e ? (int)(e - s - 1) : (int)strlen(s + 1);
        if (len > cap - 1) len = cap - 1;
        memcpy(out, s + 1, (size_t)len);
        out[len] = '\0';
        s = e ? e + 1 : s + 1 + strlen(s + 1);
    }
    else
    {
        n = ShNet_PeerCount();
        for (i = 0; i < n; i++)
        {
            const ShNetPeer* p = ShNet_Peer(i);
            int              k = p ? Soc_KnownNameAt(s, p->name) : 0;
            if (k > best) best = k;
        }
        n = ShSession_Active() ? ShSession_MemberCount() : 0;
        for (i = 0; i < n; i++)
        {
            const ShSessionMember* m = ShSession_Member(i);
            int                    k = m ? Soc_KnownNameAt(s, m->name) : 0;
            if (k > best) best = k;
        }
        for (i = 0; i < PC_ONLINE_BLOCK_MAX; i++)
        {
            int k = g_PcConfig.onlineBlocks[i].token ? Soc_KnownNameAt(s, g_PcConfig.onlineBlocks[i].name) : 0;
            if (k > best) best = k;
        }
        if (best == 0)
        {
            while (s[best] && s[best] != ' ' && s[best] != '#') best++;
        }
        if (best > cap - 1) best = cap - 1;
        memcpy(out, s, (size_t)best);
        out[best] = '\0';
        s += best;
    }
    if (*s == '#' && isdigit((unsigned char)s[1]))
    {
        int k = 1, len = (int)strlen(out);
        while (isdigit((unsigned char)s[k]) && k <= 4) k++;
        if (len + k < cap)
        {
            memcpy(out + len, s, (size_t)k);
            out[len + k] = '\0';
        }
        s += k;
    }
    return Soc_SkipSpace(s);
}

static void Soc_Lookup(int kind, const char* query, const char* arg)
{
    int i;
    for (i = 0; i < SOC_PEND_MAX && s_pend[i].used; i++) {}
    if (i == SOC_PEND_MAX)
    {
        Soc_Say("Too many commands waiting for the server. Try again in a moment.");
        return;
    }
    if (ShNet_Status() != SHNET_ST_CONNECTED)
    {
        Soc_Say("Not connected to the server.");
        return;
    }
    s_pend[i].used = 1;
    s_pend[i].req  = s_nextReq++;
    if (s_nextReq > 60000) s_nextReq = 1;
    s_pend[i].kind = kind;
    s_pend[i].ms   = SDL_GetTicks();
    snprintf(s_pend[i].query, sizeof(s_pend[i].query), "%s", query);
    snprintf(s_pend[i].arg, sizeof(s_pend[i].arg), "%s", arg ? arg : "");
    if (!ShNet_SendLookup(s_pend[i].req, query))
    {
        s_pend[i].used = 0;
        Soc_Say("Not connected to the server.");
    }
}

static void Soc_Help(const char* what)
{
    size_t i;
    what = Soc_SkipSpace(what);
    if (*what == '/') what++;
    if (!*what)
    {
        Soc_Say("Commands: /invite /whisper /join /kick /block /blocklist /unblock /report /color /accept /decline /help");
        Soc_Say("Type /help <command> for more about one.");
        return;
    }
    if (!SDL_strcasecmp(what, "w") || !SDL_strcasecmp(what, "msg")) what = "whisper";
    if (!SDL_strcasecmp(what, "colour")) what = "color";
    for (i = 0; i < sizeof(kCmds) / sizeof(kCmds[0]); i++)
    {
        if (!SDL_strcasecmp(what, kCmds[i].name))
        {
            Soc_Say("%s - %s", kCmds[i].usage, kCmds[i].help);
            return;
        }
    }
    Soc_Say("No command /%s. Type /help for the list.", what);
}

static void Soc_BlockList(void)
{
    int i, n = 0;
    for (i = 0; i < PC_ONLINE_BLOCK_MAX; i++)
    {
        if (g_PcConfig.onlineBlocks[i].token)
        {
            n++;
            Soc_Say("%d. %s", n, g_PcConfig.onlineBlocks[i].name);
        }
    }
    if (n == 0) Soc_Say("You have not blocked anyone.");
    else        Soc_Say("/unblock <name or number> lifts one.");
}

static void Soc_Unblock(const char* arg)
{
    int          i, n = 0, hit = -1, matches = 0;
    char*        end;
    long         num;
    unsigned int tok;
    arg = Soc_SkipSpace(arg);
    if (!*arg) { Soc_Say("Usage: /unblock <name or number>"); return; }
    num = strtol(arg, &end, 10);
    tok = (unsigned int)strtoul(arg, NULL, 16);
    for (i = 0; i < PC_ONLINE_BLOCK_MAX; i++)
    {
        if (!g_PcConfig.onlineBlocks[i].token) continue;
        n++;
        if ((*end == '\0' && num == n) || g_PcConfig.onlineBlocks[i].token == tok)
        {
            hit = i;
            matches = 1;
            break;
        }
        if (!SDL_strcasecmp(g_PcConfig.onlineBlocks[i].name, arg))
        {
            hit = i;
            matches++;
        }
    }
    if (hit < 0) { Soc_Say("No one called \"%s\" is on your block list (see /blocklist).", arg); return; }
    if (matches > 1) { Soc_Say("More than one blocked player is called %s: use the number from /blocklist.", arg); return; }
    Soc_Say("Unblocked %s.", g_PcConfig.onlineBlocks[hit].name);
    SH_DBG("[SOCIAL] unblocked %s (%08x)", g_PcConfig.onlineBlocks[hit].name, g_PcConfig.onlineBlocks[hit].token);
    g_PcConfig.onlineBlocks[hit].token   = 0;
    g_PcConfig.onlineBlocks[hit].name[0] = '\0';
    Soc_SaveBlock(hit);
    Soc_PushBlocks();
}

/* A room member by name (name#tag where two share it), for /kick. */
static const ShSessionMember* Soc_Member(const char* query, int* outShared)
{
    char                   name[SHNET_QUERY_MAX];
    const char*            hash;
    int                    tag = 0, i, n = ShSession_MemberCount(), hits = 0;
    const ShSessionMember* hit = NULL;
    snprintf(name, sizeof(name), "%s", query);
    hash = strrchr(name, '#');
    if (hash && isdigit((unsigned char)hash[1]))
    {
        tag = atoi(hash + 1);
        name[hash - name] = '\0';
    }
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (!m || SDL_strcasecmp(m->name, name) != 0) continue;
        if (tag && m->tag != tag) continue;
        hit = m;
        hits++;
    }
    *outShared = hits > 1;
    return hits == 1 ? hit : NULL;
}

static void Soc_Kick(const char* arg)
{
    char                   who[SHNET_QUERY_MAX];
    int                    shared = 0;
    const ShSessionMember* m;
    Soc_TakeName(arg, who, (int)sizeof(who));
    if (!who[0]) { Soc_Say("Usage: /kick <name>"); return; }
    if (!ShSession_Active() || !ShSession_IsHost())
    {
        Soc_Say("You can only kick players from a game or lobby you are hosting.");
        return;
    }
    m = Soc_Member(who, &shared);
    if (!m)
    {
        if (shared) Soc_Say("More than one player in your game is called %s: add their #number (see the player list).", who);
        else        Soc_Say("No one called %s is in your game.", who);
        return;
    }
    if (m->steamId == ShSession_SelfId()) { Soc_Say("You can't kick yourself."); return; }
    SH_DBG("[SOCIAL] /kick %s", m->name);
    ShSession_RequestKick(m->steamId);
    Soc_Say("Removed %s from your game.", m->name);
}

static void Soc_Join(const char* arg)
{
    int code = atoi(Soc_SkipSpace(arg));
    if (code < 1000 || code > 9999) { Soc_Say("Usage: /join <4-digit room code>"); return; }
    ShNetInvite_JoinCode(code, NULL);
}

static void Soc_Color(const char* arg)
{
    float       r, g, b;
    char        who[SHNET_QUERY_MAX];
    const char* rest;
    arg = Soc_SkipSpace(arg);
    if (!*arg) { Soc_Help("color"); return; }
    if (Soc_ParseColor(arg, &r, &g, &b))
    {
        snprintf(g_PcConfig.onlineColor, sizeof(g_PcConfig.onlineColor), "%s", arg);
        PcConfig_SaveKeyValue("online_color", g_PcConfig.onlineColor);
        Soc_SetColor(Soc_SelfId(), r, g, b);
        Soc_Say("Your name now shows in %s on your screen.", arg);
        return;
    }
    rest = Soc_TakeName(arg, who, (int)sizeof(who));
    if (!who[0] || !Soc_ParseColor(rest, &r, &g, &b))
    {
        Soc_Say("Unknown colour. Try red, orange, yellow, lime, green, teal, cyan, sky, blue, purple, magenta, pink, gold, white or #RRGGBB.");
        return;
    }
    Soc_Lookup(CMD_COLOR, who, rest);
}

int ShSocial_Command(const char* line)
{
    char        cmd[24];
    const char* p = line;
    int         n = 0;
    if (!line || line[0] != '/') return 0;
    p++;
    while (*p && *p != ' ' && n < (int)sizeof(cmd) - 1) cmd[n++] = *p++;
    cmd[n] = '\0';
    p = Soc_SkipSpace(p);

    if (!SDL_strcasecmp(cmd, "help") || !strcmp(cmd, "?")) { Soc_Help(p); return 1; }
    if (!SDL_strcasecmp(cmd, "blocklist")) { Soc_BlockList(); return 1; }
    if (!SDL_strcasecmp(cmd, "unblock"))   { Soc_Unblock(p); return 1; }
    if (!SDL_strcasecmp(cmd, "kick"))      { Soc_Kick(p); return 1; }
    if (!SDL_strcasecmp(cmd, "join"))      { Soc_Join(p); return 1; }
    if (!SDL_strcasecmp(cmd, "color") || !SDL_strcasecmp(cmd, "colour")) { Soc_Color(p); return 1; }
    if (!SDL_strcasecmp(cmd, "accept"))
    {
        if (!ShNetInvite_Incoming(NULL, 0)) Soc_Say("No invite is waiting for you.");
        else                                Pc_CoopMenu_OpenInviteAnswer();
        return 1;
    }
    if (!SDL_strcasecmp(cmd, "decline"))
    {
        if (!ShNetInvite_Incoming(NULL, 0)) Soc_Say("No invite is waiting for you.");
        else                                ShNetInvite_Decline();
        return 1;
    }
    if (!SDL_strcasecmp(cmd, "invite") || !SDL_strcasecmp(cmd, "block") || !SDL_strcasecmp(cmd, "whisper") ||
        !SDL_strcasecmp(cmd, "w") || !SDL_strcasecmp(cmd, "msg") || !SDL_strcasecmp(cmd, "report"))
    {
        char        who[SHNET_QUERY_MAX];
        const char* rest = Soc_TakeName(p, who, (int)sizeof(who));
        const int   kind = !SDL_strcasecmp(cmd, "invite") ? CMD_INVITE
                         : !SDL_strcasecmp(cmd, "block")  ? CMD_BLOCK
                         : !SDL_strcasecmp(cmd, "report") ? CMD_REPORT : CMD_WHISPER;
        if (!who[0])
        {
            Soc_Help(kind == CMD_INVITE ? "invite" : kind == CMD_BLOCK ? "block" : kind == CMD_REPORT ? "report" : "whisper");
            return 1;
        }
        if (kind == CMD_WHISPER && !*rest) { Soc_Say("Usage: /whisper <name> <message>"); return 1; }
        if (kind == CMD_REPORT)
        {
            char reason[SHNET_REPORT_MAX];
            snprintf(reason, sizeof(reason), "%s", rest);
            if (reason[0] == '"')
            {
                size_t l = strlen(reason);
                memmove(reason, reason + 1, l);
                l--;
                if (l > 0 && reason[l - 1] == '"') reason[l - 1] = '\0';
            }
            if (!reason[0]) { Soc_Say("Usage: /report <name> \"reason\""); return 1; }
            Soc_Lookup(kind, who, reason);
            return 1;
        }
        Soc_Lookup(kind, who, rest);
        return 1;
    }
    Soc_Say("No command /%s. Type /help for the list.", cmd);
    return 1;
}

/* A lookup came back: finish the command that asked. */
static void Soc_Finish(int kind, const char* query, const char* arg, const ShNetLookup* l)
{
    char who[SHNET_NAME_MAX + 8];
    if (l->status == SHNET_LOOK_NOTFOUND) { Soc_Say("No player called %s is online.", query); return; }
    if (l->status == SHNET_LOOK_AMBIGUOUS)
    {
        Soc_Say("%d players are called %s: add their #number, like %s#1234 (the player list, F11, shows it).",
                l->matches, query, query);
        return;
    }
    if (l->status == SHNET_LOOK_SELF && kind != CMD_COLOR) { Soc_Say("That's you."); return; }
    ShSocial_FormatName(l->name, l->tag, (l->rf & SHNET_RF_DUPNAME) != 0, who, (int)sizeof(who));
    switch (kind)
    {
    case CMD_INVITE:
        if (ShSocial_BlockedToken(l->token)) { Soc_Say("You have blocked %s. /unblock them first.", who); return; }
        ShNetInvite_SendTo(l->playerId, who, (l->rf & SHNET_RF_SPLIT) ? 2 : 1);
        break;
    case CMD_WHISPER:
        if (ShSocial_BlockedToken(l->token)) { Soc_Say("You have blocked %s. /unblock them first.", who); return; }
        {
            /* A long message goes as up to three lines, broken at a space. */
            const char* p = arg;
            int         pieces = 0;
            while (*p && pieces < SHNET_CHAT_BURST)
            {
                char piece[SHNET_CHAT_MAX];
                int  len = (int)strlen(p);
                if (len > SHNET_CHAT_MAX - 10)
                {
                    int k = SHNET_CHAT_MAX - 10;
                    while (k > SHNET_CHAT_MAX / 2 && p[k] != ' ') k--;
                    len = (p[k] == ' ') ? k : SHNET_CHAT_MAX - 10;
                }
                memcpy(piece, p, (size_t)len);
                piece[len] = '\0';
                ShNet_SendWhisper(l->playerId, piece);
                p = Soc_SkipSpace(p + len);
                pieces++;
            }
        }
        break;
    case CMD_BLOCK:
        Soc_Block(l->token, l->name);
        break;
    case CMD_REPORT:
        ShNet_SendReport(l->playerId, arg);
        SH_DBG("[SOCIAL] reported %s", who);
        break;
    case CMD_COLOR:
    {
        float r, g, b;
        if (Soc_ParseColor(arg, &r, &g, &b))
        {
            if (l->status == SHNET_LOOK_SELF)
            {
                snprintf(g_PcConfig.onlineColor, sizeof(g_PcConfig.onlineColor), "%s", arg);
                PcConfig_SaveKeyValue("online_color", g_PcConfig.onlineColor);
            }
            Soc_SetColor(l->playerId, r, g, b);
            Soc_Say("%s now shows in %s on your screen.", who, arg);
        }
        break;
    }
    default:
        break;
    }
}

void ShSocial_Tick(void)
{
    ShNetLookup  l;
    ShNetWhisper w;
    const unsigned int now = SDL_GetTicks();
    int i;

    /* Again every few seconds: the session layer may not be up on the first
     * frames, and the room link sends only when the list really changed. */
    if (!s_blocksPushed || now - s_blocksPushedMs > 5000u) Soc_PushBlocks();

    while (ShNet_TakeLookup(&l))
    {
        for (i = 0; i < SOC_PEND_MAX; i++)
        {
            if (s_pend[i].used && s_pend[i].req == l.req)
            {
                s_pend[i].used = 0;
                Soc_Finish(s_pend[i].kind, s_pend[i].query, s_pend[i].arg, &l);
                break;
            }
        }
    }
    for (i = 0; i < SOC_PEND_MAX; i++)
    {
        if (s_pend[i].used && now - s_pend[i].ms > SOC_LOOKUP_MS)
        {
            s_pend[i].used = 0;
            Soc_Say("The server did not answer. Try again.");
        }
    }

    while (ShNet_TakeWhisper(&w))
    {
        /* Theirs, from someone we blocked: dropped, as if never sent. */
        if (w.dir == SHNET_PM_IN && ShSocial_BlockedToken(w.token)) continue;
        ShSocial_ShowWhisper(w.dir, w.name, w.tag, (w.rf & SHNET_RF_DUPNAME) != 0, w.text);
    }
}
