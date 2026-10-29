/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo.c - phrase tables, marker text, and the composer state machine.
 *
 * See sh_net_memo.h for why messages are templated rather than typed.
 *
 * The word list is ORDERED BY GROUP and the group boundaries are a separate
 * table of first-indices. That is what lets the picker page by category
 * instead of scrolling sixty-odd entries one at a time, and it means adding a
 * word is a one-line change as long as it goes in its group.
 *
 * INDICES ARE THE WIRE FORMAT. A memo placed today is stored on the server as
 * (phrase 7, word 31) forever, so REORDERING EITHER TABLE REWRITES EVERY MEMO
 * ANYONE HAS EVER LEFT. Append only.
 */

#include "game.h"

#include <stdio.h>
#include <string.h>

#include "sh_net.h"
#include "sh_net_memo.h"
#include "pc_config.h"
#include "sh_log.h"

/* Reading distance. Close enough that you have to walk up to a marker, far
 * enough that you do not have to stand exactly on it. */
#define MEMO_READ_DIST Q12(1.6f)

/* How long "Message left." stays up after a placement. */
#define MEMO_PLACED_FRAMES 150

#include "sh_net_memo_text.h" /* the tables, shared with the server so it can print a marker */

#define PHRASE_COUNT ((int)(sizeof(s_phrases) / sizeof(s_phrases[0])))
#define WORD_COUNT   ((int)(sizeof(s_words)   / sizeof(s_words[0])))
/* The server refuses indices at or past these (sh_net_proto.h); raise them
 * there, and redeploy, before a table outgrows them. */
typedef char shnet_memo_phrases_fit[(PHRASE_COUNT <= SHNET_MEMO_PHRASE_MAX) ? 1 : -1];
typedef char shnet_memo_words_fit[(WORD_COUNT <= SHNET_MEMO_WORD_MAX) ? 1 : -1];
#define GROUP_COUNT  ((int)(sizeof(s_groups)  / sizeof(s_groups[0])))

int ShNetMemo_PhraseCount(void)
{
    return PHRASE_COUNT;
}

const char* ShNetMemo_Phrase(int i)
{
    return (i >= 0 && i < PHRASE_COUNT) ? s_phrases[i] : "";
}

int ShNetMemo_WordCount(void)
{
    return WORD_COUNT;
}

const char* ShNetMemo_Word(int i)
{
    return (i >= 0 && i < WORD_COUNT) ? s_words[i] : "";
}

const char* ShNetMemo_WordGroup(int i)
{
    int g;
    for (g = 0; g < GROUP_COUNT; g++)
    {
        if (s_groups[g].first == i)
        {
            return s_groups[g].name;
        }
    }
    return NULL;
}

/* Substitutes the word into the phrase by hand rather than through snprintf:
 * the phrase strings are data, and handing user-reachable data to a printf
 * format string is how a table typo becomes a crash. */
static void ShNetMemo_Format(int phrase, int word, char* out, int cap)
{
    const char* p = ShNetMemo_Phrase(phrase);
    const char* w = ShNetMemo_Word(word);
    int         o = 0;

    if (cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (phrase < 0 || phrase >= PHRASE_COUNT || word < 0 || word >= WORD_COUNT)
    {
        return;
    }

    while (*p && o < cap - 1)
    {
        if (p[0] == '%' && p[1] == 's')
        {
            const char* q = w;
            while (*q && o < cap - 1)
            {
                out[o++] = *q++;
            }
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

void ShNetMemo_Text(const ShNetMemo* m, char* out, int cap)
{
    if (!m || cap <= 0)
    {
        if (out && cap > 0) out[0] = '\0';
        return;
    }
    /* kind byte: low nibble = marker kind, high nibble = emote (0 = none). */
    switch (m->kind & 0x0F)
    {
    case SHNET_MARK_DEATH:
        snprintf(out, (size_t)cap, "%s died here",
                 m->owner[0] ? m->owner : "Someone");
        return;
    case SHNET_MARK_SAVE:
        snprintf(out, (size_t)cap, "%s rested here",
                 m->owner[0] ? m->owner : "Someone");
        return;
    default:
    {
        const char* emote = ShNetMemo_Emote((m->kind >> 4) & 0x0F);
        char        body[96];
        ShNetMemo_Format((int)m->phraseA, (int)m->wordA, body, (int)sizeof(body));
        if (emote && emote[0]) snprintf(out, (size_t)cap, "%s  %s", emote, body);
        else                   snprintf(out, (size_t)cap, "%s", body);
        return;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

int ShNetMemo_NearestReadable(void)
{
    const VECTOR3* self;
    int            count = ShNet_MemoCount();
    int            best  = -1;
    s32            bestD = 0;
    int            i;

    if (!g_PcConfig.onlineMemos || count <= 0)
    {
        return -1;
    }
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay)
    {
        return -1;
    }
    self = &g_SysWork.playerWork.player.position;

    for (i = 0; i < count; i++)
    {
        const ShNetMemo* m = ShNet_Memo(i);
        s32              dx, dz, d;

        if (!m)
        {
            continue;
        }
        dx = ABS(m->x - self->vx);
        dz = ABS(m->z - self->vz);
        if (dx > MEMO_READ_DIST || dz > MEMO_READ_DIST)
        {
            continue;
        }
        /* Chebyshev distance is enough to rank two markers the player is
         * already standing between; a real magnitude would need a square root
         * per marker per frame to pick the same one. */
        d = (dx > dz) ? dx : dz;
        if (best < 0 || d < bestD)
        {
            best  = i;
            bestD = d;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Composer                                                            */
/* ------------------------------------------------------------------ */

static int s_active;
static int s_col;      /* 0 = phrase, 1 = word, 2 = emote */
static int s_phrase;
static int s_word;
static int s_emote;    /* index into s_emotes; 0 = none */
static int s_placedTimer;

#define EMOTE_COUNT ((int)(sizeof(s_emotes) / sizeof(s_emotes[0])))

int ShNetMemo_EmoteCount(void)
{
    return EMOTE_COUNT;
}

const char* ShNetMemo_Emote(int i)
{
    return (i >= 0 && i < EMOTE_COUNT) ? s_emotes[i] : "";
}

int ShNetMemo_ComposerEmote(void)
{
    return s_emote;
}

int ShNetMemo_ComposerActive(void)
{
    return s_active;
}

void ShNetMemo_ComposerClose(void)
{
    s_active = 0;
}

void ShNetMemo_ComposerToggle(void)
{
    if (s_active)
    {
        s_active = 0;
        return;
    }
    if (!g_PcConfig.onlineEnabled || !g_PcConfig.onlineMemos)
    {
        return;
    }
    if (!ShNet_CoopLivingWorld())
    {
        ShNet_PostLocalEvent("Messages are off in this game (the host keeps it private)");
        return;
    }
    if (ShNet_Status() != SHNET_ST_CONNECTED)
    {
        return;
    }
    if (!(ShNet_ServerFlags() & SHNET_SF_MEMOS))
    {
        return;
    }
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay)
    {
        return;
    }
    s_active = 1;
    s_col    = 0;
}

int ShNetMemo_ComposerColumn(void)
{
    return s_col;
}

int ShNetMemo_ComposerPhrase(void)
{
    return s_phrase;
}

int ShNetMemo_ComposerWord(void)
{
    return s_word;
}

void ShNetMemo_ComposerPreview(char* out, int cap)
{
    char body[96];
    ShNetMemo_Format(s_phrase, s_word, body, (int)sizeof(body));
    if (s_emote > 0) snprintf(out, (size_t)cap, "%s  %s", s_emotes[s_emote], body);
    else             snprintf(out, (size_t)cap, "%s", body);
}

int ShNetMemo_JustPlaced(void)
{
    return s_placedTimer > 0;
}

void ShNetMemo_Tick(void)
{
    if (s_placedTimer > 0)
    {
        s_placedTimer--;
    }
}

/* Left/Right jumps a whole group in the word column. Sixty-odd words one at a
 * time is not a menu anyone will use twice. */
static void ShNetMemo_WordGroupStep(int dir)
{
    int g, cur = 0;
    for (g = 0; g < GROUP_COUNT; g++)
    {
        if (s_word >= s_groups[g].first)
        {
            cur = g;
        }
    }
    cur += dir;
    if (cur < 0)             cur = GROUP_COUNT - 1;
    if (cur >= GROUP_COUNT)  cur = 0;
    s_word = s_groups[cur].first;
}

void ShNetMemo_ComposerInput(int up, int down, int left, int right,
                             int confirm, int cancel)
{
    if (!s_active)
    {
        return;
    }

    if (cancel)
    {
        s_active = 0;
        return;
    }

    /* Three columns, Left/Right between them, Up/Down within. Enter advances
     * phrase -> word -> emote and leaves the message from the emote column
     * (an emote is optional: "none" is the first entry). */
    if (s_col == 0)
    {
        if (up)    s_phrase = (s_phrase + PHRASE_COUNT - 1) % PHRASE_COUNT;
        if (down)  s_phrase = (s_phrase + 1) % PHRASE_COUNT;
        if (right) s_col    = 1;
    }
    else if (s_col == 1)
    {
        if (up)    s_word = (s_word + WORD_COUNT - 1) % WORD_COUNT;
        if (down)  s_word = (s_word + 1) % WORD_COUNT;
        if (left)  s_col  = 0;
        if (right) s_col  = 2;
    }
    else
    {
        if (up)    s_emote = (s_emote + EMOTE_COUNT - 1) % EMOTE_COUNT;
        if (down)  s_emote = (s_emote + 1) % EMOTE_COUNT;
        if (left)  s_col   = 1;
    }

    if (confirm)
    {
        if (s_col < 2)
        {
            s_col++;
        }
        else
        {
            const VECTOR3* p = &g_SysWork.playerWork.player.position;
            if (ShNet_PlaceMarker(SHNET_MARK_MEMO | (s_emote << 4),
                                  (int)p->vx, (int)p->vy, (int)p->vz,
                                  (short)g_SysWork.playerWork.player.rotation.vy,
                                  (unsigned short)s_phrase, (unsigned short)s_word,
                                  0xFFFF, 0xFFFF))
            {
                char text[96];
                ShNetMemo_Format(s_phrase, s_word, text, (int)sizeof(text));
                SH_DBG("[NET] left a message: \"%s %s\"", s_emotes[s_emote], text);
                s_placedTimer = MEMO_PLACED_FRAMES;
                /* The server assigns the id and hands it back in the next
                 * marker list, so ask for one instead of guessing. */
                ShNet_RequestMemos();
            }
            s_active = 0;
        }
    }
}
