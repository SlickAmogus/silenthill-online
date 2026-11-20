/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo.h - the messages players leave for each other.
 *
 * A TEMPLATE WITH ONE WORD IN IT. A memo is a phrase index plus a word, and
 * the word is either an index into the table or, on a server that takes them
 * (SHNET_SF_MEMO2), up to SHNET_MEMO_TEXT_MAX typed characters. The phrase is
 * always from the table, so a message stays a sentence about the place it was
 * left in, and the one typed word is slur-masked by the game and again by the
 * server (sh_net_chat_filter.h), which is the same moderation chat has.
 *
 * The tables live here rather than on the server: the server stores indices,
 * so a client with a translated table shows the same memo in another language
 * without the server knowing anything about it. A typed word is shown as it
 * was typed.
 *
 * Messages do not last: seven days, and one more for each like (the server's
 * rule, SHNET_MEMO_LIFE_S). Accept likes the one under the player's feet and
 * cancel dislikes it.
 *
 * No GL in this file. The composer is a state machine the UI draws
 * (sh_net_ui.c) and the game thread drives.
 */
#ifndef SH_NET_MEMO_H
#define SH_NET_MEMO_H

#include "sh_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

int         ShNetMemo_PhraseCount(void);
const char* ShNetMemo_Phrase(int i);     /* display form, "%s" where the word goes */

int         ShNetMemo_WordCount(void);
const char* ShNetMemo_Word(int i);
/* Words are grouped so the picker is navigable; this is the heading the group
 * containing word `i` sits under, or NULL when `i` is not the first of a
 * group. */
const char* ShNetMemo_WordGroup(int i);

/* Render a marker to one display line. Handles the non-memo kinds too (a death
 * or save marker has no phrase, only an owner). */
void ShNetMemo_Text(const ShNetMemo* m, char* out, int cap);

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Index of the nearest marker the player is standing close enough to read, or
 * -1. Reads the player's position itself; call from the game thread. */
int ShNetMemo_NearestReadable(void);

/* ------------------------------------------------------------------ */
/* Composing                                                           */
/* ------------------------------------------------------------------ */

int  ShNetMemo_ComposerActive(void);
void ShNetMemo_ComposerToggle(void);
void ShNetMemo_ComposerClose(void);

/* One frame of edge-triggered input. Placing the memo (confirm on the last
 * column) closes the composer and queues the marker at the player's feet. */
void ShNetMemo_ComposerInput(int up, int down, int left, int right,
                             int confirm, int cancel);

/* What the composer is showing, for the panel. */
int         ShNetMemo_ComposerColumn(void);  /* 0 = phrase, 1 = word, 2 = emote */

/* Emotes: a short tag shown with a message, carried in the memo kind byte's
 * high nibble. 0 = none. */
int         ShNetMemo_EmoteCount(void);
const char* ShNetMemo_Emote(int i);
int         ShNetMemo_ComposerEmote(void);
int         ShNetMemo_ComposerPhrase(void);
int         ShNetMemo_ComposerWord(void);
void        ShNetMemo_ComposerPreview(char* out, int cap);

/* Non-zero for a few seconds after a placement, so the UI can say so. A pure
 * read: the draw path asks more than once a frame. ShNetMemo_Tick, once a
 * frame from the game thread, is what makes it expire. */
/* The emoji (0..SHNET_EMOJI_COUNT-1, sh_net_memo_text.h) a marker or the
 * composer carries, or -1 for none. */
int         ShNetMemo_MemoEmoji(const ShNetMemo* m);
int         ShNetMemo_ComposerEmoji(void);

/* The pointer's way in (sh_net_ui.c): a column's selection and size, pick or
 * step an entry (which also focuses that column), leave the message. */
int         ShNetMemo_ComposerIndex(int col);
int         ShNetMemo_ComposerCount(int col);
void        ShNetMemo_ComposerSet(int col, int idx);
void        ShNetMemo_ComposerStep(int col, int delta);
void        ShNetMemo_ComposerPlace(void);
/* Confirm on a column: the next one, the keyboard on "type your own", or the
 * message left from the last. */
void        ShNetMemo_ComposerAdvance(int col);

/* The typed word. The word column lists it first when the server takes typed
 * words: Base is then 1 and the column's entry n is table word n - 1, with
 * entry 0 the typed one; 0 on a server from before them. */
int         ShNetMemo_ComposerWordBase(void);
const char* ShNetMemo_ComposerTyped(void);  /* as it stands, while typing too */
int         ShNetMemo_ComposerTyping(void); /* the keyboard is up for it */
void        ShNetMemo_ComposerType(void);

/* ------------------------------------------------------------------ */
/* Likes and dislikes                                                  */
/* ------------------------------------------------------------------ */

/* The emoji a vote is shown with, by their place on the emoji sheet
 * (sh_net_memo_text.h: that table only ever grows at its end). */
#define SHNET_EMOJI_THUMBS_UP   16
#define SHNET_EMOJI_THUMBS_DOWN 17

/* 1 when this player may still vote on that marker: someone else's message,
 * not voted on yet, on a server that counts votes. */
int ShNetMemo_Votable(const ShNetMemo* m);
/* One frame's accept and cancel presses during play. A message underfoot that
 * can be voted on takes one of them as a like or a dislike; returns 1 or 2
 * for the press it used up, which the caller removes from the pad. */
int ShNetMemo_VoteInput(int accept, int cancel);
/* The pad bits that vote: the game's accept (like = 1) or cancel (like = 0). */
unsigned int ShNetMemo_VoteButton(int like);

int         ShNetMemo_JustPlaced(void);
void        ShNetMemo_Tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_MEMO_H */
