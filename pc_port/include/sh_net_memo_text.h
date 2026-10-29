/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo_text.h - the living world message tables: phrases, words, word
 * groups and emoji. Included by the client (sh_net_memo.c, which pages and
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
    "Thank you for %s",
    /* 24 */
    "Watch out for %s",
    "Don't go near %s",
    "Go to %s",
    "I found %s",
    "I lost %s",
    "Use %s",
    "Run from %s",
    "Kill %s",
    "Avoid %s",
    "%s is behind you",
    "%s is close",
    "Check %s",
    "Hurry to %s",
    "I miss %s",
    "Where is %s?",
    "Is that %s?",
    "Pray for %s",
    "Listen for %s",
    "%s is a lie",
    "Ha ha. %s",
    "Ugh. %s",
    "Help me with %s",
    "Did you see %s?",
    "Good luck with %s",
    "Save before %s",
    "Don't forget %s",
    "Sorry about %s",
    "Welcome to %s"
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
    "the truth", "a dream", "the end", "a secret",
    /* 74: people. New groups go on the end: a word inserted inside an older
     * group would renumber every word after it. */
    "Harry", "Cheryl", "Cybil", "Dahlia", "Kaufmann", "Lisa", "Alessa",
    "the cult", "a friend", "a stranger", "you", "me", "everyone", "nobody",
    /* 88: monsters */
    "the Air Screamer", "the Groaner", "the Romper", "the Hanged Scratcher",
    "the Split Worm", "the Twinfeeler", "the Floatstinger", "the larval stalker",
    "the mumbler", "the boss", "the creeper", "the night flutter",
    /* 100: items */
    "the steel pipe", "the chainsaw", "the katana", "the rock drill",
    "the hyper blaster", "an ampoule", "the Flauros", "Aglaophotis",
    "a puzzle", "a clue", "a code", "a save point",
    "a shortcut", "a secret door", "the plastic bottle", "the channeling stone",
    /* 116: landmarks */
    "the Balkan Church", "Midwich Elementary", "Alchemilla Hospital", "Annie's Bar",
    "Green Lion Antiques", "the police station", "the Indian Runner", "Norman's Motel",
    "the water works", "Cafe 5to2", "Nowhere", "the basement",
    "the rooftop", "the bridge", "the boat", "the gas station",
    /* 132: which way */
    "the left", "the right", "upstairs", "downstairs",
    "behind you", "straight ahead", "around the corner", "this way",
    "the other way", "back there",
    /* 142: doing */
    "saving", "healing", "reloading", "fighting",
    "sprinting", "exploring", "dying", "praying",
    "crying", "screaming", "laughing", "giving up",
    /* 154: mood */
    "fear", "pain", "darkness", "light",
    "luck", "courage", "rest", "danger",
    "regret", "madness", "peace", "home"
};

static const struct { int first; const char* name; } s_groups[] SHMT_UNUSED = {
    {  0, "THEY" },
    { 10, "SUPPLIES" },
    { 22, "THE PLACE" },
    { 38, "WHAT TO DO" },
    { 50, "WHERE" },
    { 62, "WHAT I FELT" },
    { 74, "PEOPLE" },
    { 88, "MONSTERS" },
    { 100, "ITEMS" },
    { 116, "LANDMARKS" },
    { 132, "WHICH WAY" },
    { 142, "DOING" },
    { 154, "MOOD" }
};

/* The emoji (sh_net_emoji_png.h holds their pictures, in this order). APPEND
 * ONLY, like the words: a marker stores the index. It rides wordB with
 * phraseB left SHNET_MEMO_NONE, a pair no second clause can be, so the
 * server, which checks each half on its own, takes it as it is. */
static const char* const s_emoji[] SHMT_UNUSED = {
    "neutral", "smile", "sad", "angry", "cry", "shock", "wink", "dead",
    "blush", "cool", "sweat", "hurt", "confused", "alarmed", "love", "smoke",
    "thumbs up", "thumbs down", "peace", "middle finger", "wave", "point", "fist", "pray",
    "first aid", "flashlight", "handgun", "knife", "bloody hand", "key", "map", "radio",
    "fog", "street lamp", "welcome sign", "Pyramid Head", "nurse", "shadow", "creature", "spiral",
    "otherworld", "church", "car", "tunnel", "cross", "street", "window", "apartment",
    "lost", "nope", "skull", "heart", "heartbreak", "sleepy", "happy", "smug"
};
#define SHNET_EMOJI_COUNT ((int)(sizeof(s_emoji) / sizeof(s_emoji[0])))

/* Markers left before the emoji carried an ASCII emote in the high nibble of
 * their kind byte (1..14); each shows as the emoji nearest it. */
static const signed char s_emojiLegacy[15] SHMT_UNUSED = {
    -1, 51, 13, 12, 1, 2, 54, 55, 21, 50, 20, 0, 53, 5, 39
};

/* The emoji a marker carries, or -1. */
SHMT_UNUSED static int ShNetMemo_EmojiOf(int kind, int phraseB, int wordB)
{
    if (phraseB == SHNET_MEMO_NONE && wordB != SHNET_MEMO_NONE)
        return (wordB >= 0 && wordB < SHNET_EMOJI_COUNT) ? wordB : -1;
    return s_emojiLegacy[(kind >> 4) & 0x0F];
}

#endif /* SH_NET_MEMO_TEXT_H */
