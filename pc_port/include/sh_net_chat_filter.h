/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_chat_filter.h - masks racial slurs in a chat line, in place.
 *
 * Shared by the server (sh_master.c, before a line is relayed or logged) and
 * the client (sh_net_chat.c, on what it sends and on what it shows, so a
 * line that came through an older server or straight from a co-op partner is
 * covered too). Letters only are compared, lower-cased, with the usual digit
 * and symbol stand-ins read as letters, so "n1gger" is "nigger". A pattern
 * marked whole must stand as its own word (a plural s or z allowed), so that
 * "raccoon" and "spicy" pass. ASCII only, like the chat itself.
 */
#ifndef SH_NET_CHAT_FILTER_H
#define SH_NET_CHAT_FILTER_H

#include <string.h>

#define SHNET_CF_MAX 512

typedef struct { const char* word; int whole; } ShNetChatBadWord;

static const ShNetChatBadWord s_shnetBadWords[] = {
    { "nigger", 0 }, { "nigga", 0 }, { "niggr", 0 }, { "nigglet", 0 },
    { "wetback", 0 }, { "raghead", 0 }, { "towelhead", 0 }, { "porchmonkey", 0 },
    { "jigaboo", 0 }, { "jiggaboo", 0 }, { "spearchucker", 0 }, { "zipperhead", 0 },
    { "chinaman", 0 }, { "sandnigger", 0 },
    { "kike", 1 }, { "kyke", 1 }, { "chink", 1 }, { "gook", 1 }, { "spic", 1 }, { "spick", 1 },
    { "spik", 1 }, { "coon", 1 }, { "beaner", 1 }, { "wop", 1 }, { "dago", 1 }, { "paki", 1 },
    { "darkie", 1 }, { "darky", 1 }, { "injun", 1 }, { "redskin", 1 }, { "gyppo", 1 }
};

/* Lower-case letter for c, with the stand-ins mapped; 0 for a separator. */
static char ShNetChat_FilterLetter(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    if (c >= 'a' && c <= 'z') return (char)c;
    switch (c)
    {
    case '0': return 'o';
    case '1': case '!': case '|': return 'i';
    case '3': return 'e';
    case '4': case '@': return 'a';
    case '5': case '$': return 's';
    case '7': return 't';
    default:  return 0;
    }
}

/* Masks every match with '*'. Returns how many words were masked. */
static int ShNetChat_Censor(char* s)
{
    char norm[SHNET_CF_MAX + 2];
    int  map[SHNET_CF_MAX + 2];
    int  n = 0, i, w, hits = 0;

    if (!s) return 0;
    norm[n] = ' '; map[n] = -1; n++;
    for (i = 0; s[i] && n < SHNET_CF_MAX; i++)
    {
        char l = ShNetChat_FilterLetter((unsigned char)s[i]);
        if (l) { norm[n] = l; map[n] = i; n++; }
        else if (norm[n - 1] != ' ') { norm[n] = ' '; map[n] = -1; n++; }
    }
    if (norm[n - 1] != ' ') { norm[n] = ' '; map[n] = -1; n++; }
    norm[n] = '\0';

    for (w = 0; w < (int)(sizeof(s_shnetBadWords) / sizeof(s_shnetBadWords[0])); w++)
    {
        const char* word = s_shnetBadWords[w].word;
        const int   wl   = (int)strlen(word);
        const char* at   = norm;
        while ((at = strstr(at, word)) != NULL)
        {
            int start = (int)(at - norm), end = start + wl, k;
            if (s_shnetBadWords[w].whole)
            {
                if (norm[start - 1] != ' ') { at++; continue; }
                if (norm[end] == 's' || norm[end] == 'z') end++;
                if (norm[end] != ' ') { at++; continue; }
            }
            for (k = start; k < end; k++)
            {
                if (map[k] >= 0) s[map[k]] = '*';
            }
            hits++;
            at = norm + end;
        }
    }
    return hits;
}

#endif /* SH_NET_CHAT_FILTER_H */
