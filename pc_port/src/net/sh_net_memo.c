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
#include "sh_net_chat.h" /* the typed word borrows the chat's keyboard */
#include "pc_config.h"
#include "sh_log.h"

/* Reading distance. Close enough that you have to walk up to a marker, far
 * enough that you do not have to stand exactly on it. */
#define MEMO_READ_DIST Q12(0.8f)

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
 * the phrase strings are data, and the word may be one a player typed. Handing
 * either to a printf format string is how a typo, or a stranger, becomes a
 * crash. */
static void ShNetMemo_FormatWith(int phrase, const char* w, char* out, int cap)
{
    const char* p = ShNetMemo_Phrase(phrase);
    int         o = 0;

    if (cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (phrase < 0 || phrase >= PHRASE_COUNT || !w)
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

static void ShNetMemo_Format(int phrase, int word, char* out, int cap)
{
    if (cap > 0)
    {
        out[0] = '\0';
    }
    if (word >= 0 && word < WORD_COUNT)
    {
        ShNetMemo_FormatWith(phrase, ShNetMemo_Word(word), out, cap);
    }
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
        /* The emoji is a picture; the overlay draws it beside this text. */
        if (m->wordA == SHNET_MEMO_NONE)
            ShNetMemo_FormatWith((int)m->phraseA, m->text[0] ? m->text : NULL, out, cap);
        else
            ShNetMemo_Format((int)m->phraseA, (int)m->wordA, out, cap);
        return;
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

static int  s_active;
void ShNetMemo_ComposerPlace(void);
static int  s_col;      /* 0 = phrase, 1 = word, 2 = emote */
static int  s_phrase;
static int  s_word;     /* a word of the table, or -1: the one in s_typed */
static char s_typed[SHNET_MEMO_TEXT_MAX];
static int  s_emote;    /* 0 = none, else emoji s_emote - 1 */
static int  s_placedTimer;
static int  s_padHold;  /* frames the pad's confirm and cancel wait after typing ends */

/* The word column opens with "type your own" when the server keeps typed
 * words; one from before them has only the table. */
int ShNetMemo_ComposerWordBase(void)
{
    return (ShNet_ServerFlags() & SHNET_SF_MEMO2) ? 1 : 0;
}

int ShNetMemo_ComposerTyping(void)
{
    return s_active && ShNetChat_Prompting();
}

/* The typed word as it stands, while it is being typed too. */
const char* ShNetMemo_ComposerTyped(void)
{
    return ShNetMemo_ComposerTyping() ? ShNetChat_PromptText() : s_typed;
}

static void Memo_TypedDone(const char* text)
{
    int n = 0, i = 0;
    /* The ends trimmed, as the server will: what is shown here is what is left. */
    while (text[i] == ' ') i++;
    for (; text[i] && n < SHNET_MEMO_TEXT_MAX - 1; i++) s_typed[n++] = text[i];
    while (n && s_typed[n - 1] == ' ') n--;
    s_typed[n] = '\0';
    /* The Enter that ended the typing must not also be the pad's confirm. */
    s_padHold = 12;
    if (s_active && s_typed[0])
    {
        s_col = 2;
    }
}

/* The keyboard for the typed word: a real one, a phone's, the browser's. */
void ShNetMemo_ComposerType(void)
{
    if (!s_active || !ShNetMemo_ComposerWordBase() || ShNetChat_Prompting())
    {
        return;
    }
    s_col  = 1;
    s_word = -1;
    ShNetChat_PromptOpen(s_typed, SHNET_MEMO_TEXT_MAX, Memo_TypedDone);
}

#define EMOTE_COUNT (SHNET_EMOJI_COUNT + 1)

int ShNetMemo_EmoteCount(void)
{
    return EMOTE_COUNT;
}

const char* ShNetMemo_Emote(int i)
{
    return (i > 0 && i < EMOTE_COUNT) ? s_emoji[i - 1] : "";
}

int ShNetMemo_ComposerEmoji(void)
{
    return s_emote - 1;
}

int ShNetMemo_MemoEmoji(const ShNetMemo* m)
{
    if (!m || (m->kind & 0x0F) != SHNET_MARK_MEMO)
        return -1;
    return ShNetMemo_EmojiOf(m->kind, m->phraseB, m->wordB);
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
    if (ShNetMemo_ComposerTyping())
    {
        ShNetChat_Close();
    }
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
    if (s_word < 0)
        ShNetMemo_FormatWith(s_phrase, ShNetMemo_ComposerTyped(), out, cap);
    else
        ShNetMemo_Format(s_phrase, s_word, out, cap);
}

/* The columns as the panel lists them. The word column is the table with
 * "type your own" in front when the server takes typed words, so its entry n
 * is word n - base, and entry 0 is then the typed one. */
static int Memo_ColCount(int col)
{
    return col == 0 ? PHRASE_COUNT : col == 1 ? WORD_COUNT + ShNetMemo_ComposerWordBase() : EMOTE_COUNT;
}

int ShNetMemo_ComposerIndex(int col)
{
    if (col <= 0) return s_phrase;
    if (col >= 2) return s_emote;
    if (s_word < 0 && !ShNetMemo_ComposerWordBase()) s_word = 0; /* the server changed under us */
    return s_word + ShNetMemo_ComposerWordBase();
}

int ShNetMemo_ComposerCount(int col)
{
    return Memo_ColCount(col < 0 ? 0 : col > 2 ? 2 : col);
}

void ShNetMemo_ComposerSet(int col, int idx)
{
    int n;
    if (!s_active || col < 0 || col > 2 || ShNetMemo_ComposerTyping())
        return;
    n     = Memo_ColCount(col);
    idx   = ((idx % n) + n) % n;
    s_col = col;
    if (col == 0)      s_phrase = idx;
    else if (col == 1) s_word   = idx - ShNetMemo_ComposerWordBase();
    else               s_emote  = idx;
}

void ShNetMemo_ComposerStep(int col, int delta)
{
    if (col < 0 || col > 2)
        return;
    ShNetMemo_ComposerSet(col, ShNetMemo_ComposerIndex(col) + delta);
}

/* Confirm on a column: on to the next, and from the last the message is left.
 * On "type your own" it opens the keyboard instead, with what was typed
 * before, so the same press both starts a word and edits it. */
void ShNetMemo_ComposerAdvance(int col)
{
    if (!s_active || ShNetMemo_ComposerTyping())
        return;
    if (col == 1 && s_word < 0)
    {
        ShNetMemo_ComposerType();
    }
    else if (col < 2)
    {
        ShNetMemo_ComposerSet(col + 1, ShNetMemo_ComposerIndex(col + 1));
    }
    else
    {
        ShNetMemo_ComposerPlace();
    }
}

void ShNetMemo_ComposerPlace(void)
{
    const VECTOR3*       p;
    const unsigned short emoji = (unsigned short)(s_emote > 0 ? s_emote - 1 : SHNET_MEMO_NONE);
    int                  sent;
    if (!s_active || ShNetMemo_ComposerTyping())
        return;
    if (s_word < 0 && !s_typed[0])
    {
        /* "Type your own" with nothing typed yet: ask for the word rather
         * than leave a sentence with a hole in it. */
        ShNetMemo_ComposerType();
        return;
    }
    p = &g_SysWork.playerWork.player.position;
    if (s_word < 0)
        sent = ShNet_PlaceMemoTyped((int)p->vx, (int)p->vy, (int)p->vz,
                                    (short)g_SysWork.playerWork.player.rotation.vy,
                                    (unsigned short)s_phrase, s_typed, emoji);
    else
        sent = ShNet_PlaceMarker(SHNET_MARK_MEMO,
                                 (int)p->vx, (int)p->vy, (int)p->vz,
                                 (short)g_SysWork.playerWork.player.rotation.vy,
                                 (unsigned short)s_phrase, (unsigned short)s_word,
                                 SHNET_MEMO_NONE, emoji);
    if (sent)
    {
        char text[96];
        ShNetMemo_ComposerPreview(text, (int)sizeof(text));
        SH_DBG("[NET] left a message: \"%s\" [%s]%s", text, s_emote > 0 ? s_emoji[s_emote - 1] : "no emoji",
               s_word < 0 ? " (typed word)" : "");
        s_placedTimer = MEMO_PLACED_FRAMES;
        /* The server assigns the id and hands it back in the next marker
         * list, so ask for one instead of guessing. */
        ShNet_RequestMemos();
    }
    s_active = 0;
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
    if (s_padHold > 0)
    {
        s_padHold--;
    }
}

/* ------------------------------------------------------------------ */
/* Likes and dislikes                                                  */
/* ------------------------------------------------------------------ */

int ShNetMemo_Votable(const ShNetMemo* m)
{
    return m && (m->kind & 0x0F) == SHNET_MARK_MEMO && !m->own && !m->mine &&
           (ShNet_ServerFlags() & SHNET_SF_MEMO2) && ShNet_Status() == SHNET_ST_CONNECTED;
}

/* Accept likes the message under the player's feet and cancel dislikes it,
 * once. The press is used up: it returns here as 1 (accept) or 2 (cancel) and
 * the caller takes it off the pad, so it is a vote and nothing else. Without
 * that every door opened beside a message would like it, and every lamp
 * switched would not. After the vote both buttons are the game's again. */
int ShNetMemo_VoteInput(int accept, int cancel)
{
    const ShNetMemo* m;
    int              idx;

    if ((!accept && !cancel) || s_active)
    {
        return 0;
    }
    idx = ShNetMemo_NearestReadable();
    m   = (idx >= 0) ? ShNet_Memo(idx) : NULL;
    if (!ShNetMemo_Votable(m))
    {
        return 0;
    }
    ShNet_RateMemo(m->memoId, accept ? 1 : -1);
    return accept ? 1 : 2;
}

/* Left/Right jumps a whole group in the word column. Sixty-odd words one at a
 * time is not a menu anyone will use twice. */
static void ShNetMemo_WordGroupStep(int dir)
{
    int g, cur = 0;
    for (g = 0; g < GROUP_COUNT; g++)
    {
        if (s_word >= 0 && s_word >= s_groups[g].first)
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
    if (!s_active || ShNetMemo_ComposerTyping())
    {
        return;
    }
    if (s_padHold > 0)
    {
        confirm = cancel = 0;
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
        if (up)    ShNetMemo_ComposerStep(1, -1);
        if (down)  ShNetMemo_ComposerStep(1, 1);
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
        ShNetMemo_ComposerAdvance(s_col);
    }
}
