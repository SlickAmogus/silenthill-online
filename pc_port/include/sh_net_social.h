/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_social.h - chat commands, name tags, blocks, colours, speech.
 *
 * Every player online has a tag for the session, a 4-digit number the server
 * hands out ("Harry#2048"), so a command can always name exactly one person.
 * The tag is only shown where a name alone is not enough: always in the
 * player list, and in chat or over a head only when someone else there has
 * the same name.
 *
 * Blocks are this player's own: the server's address token for the blocked
 * player is kept in config with their name, and nothing about it reaches the
 * blocked player, who simply finds themselves ignored. Rooms we host turn
 * them away at the door (the server holds our list while we host).
 *
 * Name colours are local too: each player gets a random readable colour on
 * this screen; /color changes what this screen shows. Game thread only.
 */
#ifndef SH_NET_SOCIAL_H
#define SH_NET_SOCIAL_H

#ifdef __cplusplus
extern "C" {
#endif

void ShSocial_Tick(void);

/* A typed line that starts with '/': runs it and returns 1 (any answer goes to
 * the chat as a system line). 0 = not a command. */
int  ShSocial_Command(const char* line);

/* Global chat is one message per 10 seconds: seconds still to wait, 0 = go. */
int  ShSocial_GlobalWaitSec(void);
void ShSocial_GlobalSent(void);

/* "Harry" or "Harry#2048" (showTag and a tag known). */
void ShSocial_FormatName(const char* name, unsigned int tag, int showTag, char* out, int cap);

/* The colour this screen shows a player's name in, 0..1. id = the player id
 * (living world or room member; they are the same on the community server). */
void ShSocial_Color(unsigned long long id, float* r, float* g, float* b);

/* Blocked by this player: by address token, or by a player id we can find a
 * token for (the roster, the room). */
int          ShSocial_BlockedToken(unsigned int token);
int          ShSocial_BlockedId(unsigned long long id);
unsigned int ShSocial_TokenOf(unsigned long long id);

/* A line a player said in chat, shown over their head for a few seconds.
 * Speech returns NULL when there is nothing to show; *alpha fades it out. */
void        ShSocial_Speak(unsigned long long id, const char* text);
const char* ShSocial_Speech(unsigned long long id, float* alpha);

/* An incoming whisper or the echo of ours, as a chat line ("From X: ..."). */
void ShSocial_ShowWhisper(int dir, const char* name, unsigned int tag, int showTag, const char* text);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_SOCIAL_H */
