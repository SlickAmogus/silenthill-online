/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo_text.h - the living world message tables: phrases, words, word
 * groups and emotes. Included by the client (sh_net_memo.c, which pages and
 * formats them) and by the server (sh_master.c, which prints a placed marker
 * in words for the admin feed).
 *
 * INDICES ARE THE WIRE FORMAT. A memo placed today is stored on the server as
 * (phrase 7, word 31) forever, so REORDERING EITHER TABLE REWRITES EVERY MEMO
 * ANYONE HAS EVER LEFT. Append only; a word only ever at the END OF ITS GROUP.
 */
#ifndef SH_NET_MEMO_TEXT_H
#define SH_NET_MEMO_TEXT_H

#if defined(__GNUC__) || defined(__clang__)
#define SHMT_UNUSED __attribute__((unused))
#else
#define SHMT_UNUSED
#endif

/* APPEND ONLY - see the file header. */
static const char* const s_phrases[] SHMT_UNUSED = {
    "%s ahead",
    "Beware of %s",
    "Try %s",
    "Need %s",
    "No %s",
    "%s here",
    "Look for %s",
    "I can hear %s",
    "Maybe %s",
    "Turn back. %s",
    "Remember %s",
    "It was %s",
    "Why %s?",
    "So cold... %s",
    "%s",
    "Don't trust %s",
    "%s saved me",
    "%s killed me",
    "Nothing but %s",
    "Follow %s",
    "%s, I think",
    "Hidden: %s",
    "Wrong way. %s",
    "Thank you for %s"
};

/* APPEND ONLY, and only ever at the END OF ITS GROUP - see the file header. */
static const char* const s_words[] SHMT_UNUSED = {
    /* 0: creatures */
    "the birds", "the dogs", "the nurses", "the children",
    "the crawling things", "something big", "the twitching one",
    "the thing in the dark", "the doctors", "the wings above",
    /* 10: supplies */
    "a health drink", "ammo", "a first aid kit", "an antidote",
    "the handgun", "the shotgun", "the rifle", "the knife",
    "the hammer", "the axe", "a key", "the map",
    /* 22: the world */
    "the door", "the stairs", "the hole", "the elevator",
    "the corridor", "the window", "the gate", "the ladder",
    "the drain", "the locker", "the vent", "the roof",
    "the fence", "the car", "the radio", "the flashlight",
    /* 38: what to do */
    "running", "hiding", "listening", "waiting",
    "looking up", "looking down", "going back", "standing still",
    "the radio static", "turning it off", "reading it", "taking it",
    /* 50: places */
    "the school", "the hospital", "the sewer", "the church",
    "the alley", "the fog", "the other side", "the lighthouse",
    "the motel", "the mall", "the pier", "the amusement park",
    /* 62: feelings */
    "hope", "nothing", "a trap", "silence",
    "blood", "a dead end", "safety", "help",
    "the truth", "a dream", "the end", "a secret"
};

static const struct { int first; const char* name; } s_groups[] SHMT_UNUSED = {
    {  0, "THEY" },
    { 10, "SUPPLIES" },
    { 22, "THE PLACE" },
    { 38, "WHAT TO DO" },
    { 50, "WHERE" },
    { 62, "WHAT I FELT" }
};

/* Emotes ride the high nibble of the memo kind byte (no wire-size change), so
 * at most 15. ASCII only: every glyph is guaranteed in the overlay font. */
static const char* const s_emotes[] SHMT_UNUSED = {
    "", "<3", "!!", "??", ":)", ":(", "XD", "**", "->", "RIP", "o/", "...", "zZz", "!?", "<o>"
};

#endif /* SH_NET_MEMO_TEXT_H */
