/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_invite.h - inviting a living-world player into your game.
 *
 * Two players who can see each other's ghosts can play together without
 * leaving the world: the M menu offers "Invite <nearest>", the other side
 * gets "Accept" / "Decline" in theirs. On accept the inviter opens a private
 * room on the server (or uses the one they already host), sends its code, and
 * both reboot into the inviter's game as a co-op host and guest. The whole
 * conversation rides SHNET_MSG_INVITE through the living-world server.
 *
 * Rules: an invite lasts 30 seconds on both ends; the invited player can hold
 * only one at a time (others get "busy"); a declined inviter waits two
 * minutes before asking that player again. Accepting ends the invited
 * player's own game, so they may save first: the invite then stays open
 * while they are in the save screen (the inviter sees "saving" and may still
 * cancel, which also costs the two minutes; the save itself is never
 * interrupted) and the accept goes out when they are back. Game thread only.
 */
#ifndef SH_NET_INVITE_H
#define SH_NET_INVITE_H

#ifdef __cplusplus
extern "C" {
#endif

void ShNetInvite_Tick(void);

/* The living world is up, we are in a map, and we could take someone in:
 * not in a session, or hosting a server room with a free seat. */
int  ShNetInvite_CanInvite(void);
/* In a map but where the game cannot be saved (a boss is alive, or a scene is
 * playing): the Invite row shows why and does nothing, and invites that
 * arrive get "busy". */
int  ShNetInvite_Blocked(void);
/* The nearest other player on this map who is not already in our game; 0 =
 * nobody. name may be NULL. *outCooldownSec > 0 when they declined us lately. */
unsigned int ShNetInvite_Nearest(char* name, int cap, int* outCooldownSec);
/* Seconds left on the invite we sent / the invite waiting for our answer;
 * 0 when there is none. name may be NULL. */
int  ShNetInvite_Outgoing(char* name, int cap);
int  ShNetInvite_Incoming(char* name, int cap);
/* The player we invited is in their save screen before joining. */
int  ShNetInvite_OutgoingSaving(void);

void ShNetInvite_Send(void);    /* to the nearest player */
void ShNetInvite_Cancel(void);
void ShNetInvite_Accept(void);
/* Accept, but open the save screen first; the accept goes out when it closes.
 * Returns 0 (nothing happens, the invite stays) when the screen will not open. */
int  ShNetInvite_AcceptAfterSave(void);
void ShNetInvite_Decline(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_INVITE_H */
