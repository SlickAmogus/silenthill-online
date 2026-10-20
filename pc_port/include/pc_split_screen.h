/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_split_screen.h - two players on one PC.
 *
 * Split screen is co-op with both players local: Player 1's game hosts a room
 * on this PC (the same embedded relay "Host on: This PC" uses) and launches a
 * SECOND copy of the game for Player 2, which joins that room over loopback.
 * Each copy draws its own half of the screen in a borderless window and reads
 * its own input: Player 1 the keyboard, mouse and every controller but one,
 * Player 2 exactly that one controller. Everything the co-op game already does
 * (shared world, items, health bars, cutscenes, pause) applies unchanged,
 * because to the network layer this IS a co-op game with two members.
 *
 * Why two processes rather than two cameras in one: the game has one camera,
 * one player and one ordering table per frame, built into every system from
 * the renderer to the sound engine. A second viewport inside one process would
 * mean rendering the world twice per frame with the player and camera state
 * swapped between them; the co-op layer already does the sharing part, and a
 * second process gives the second view for the price of a window placement.
 *
 * Roles: 0 = a normal game, 1 = Player 1's window (the host), 2 = Player 2's.
 */
#ifndef PC_SPLIT_SCREEN_H
#define PC_SPLIT_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Startup. ParseArg consumes the split-screen options Player 1's process puts
 * on Player 2's command line (returns 1 and advances *i when argv[*i] was one).
 * The rest apply Player 2's overrides at the matching points of main(). */
int  Pc_Split_ParseArg(int argc, char** argv, int* i);
void Pc_Split_NoteDataArg(const char* path);   /* the -data the user gave, passed on */
void Pc_Split_AfterConfigLoad(void);           /* P2: window, no intros, no Steam, no saving */
void Pc_Split_AfterWindow(void);               /* P2: place the window, pick the pad */
void Pc_Split_AfterPlayAsInit(void);           /* P2: the character Player 1 chose for them */

/* Once per frame, every state. P1 watches the second window; P2 joins the room,
 * follows the host, and quits when Player 1's game is gone. */
void Pc_Split_Tick(void);

int  Pc_Split_Role(void);     /* 0 none, 1 Player 1's window, 2 Player 2's */

/* The menu (pc_coop_menu.c). The attached game controllers, numbered 0..n-1 in
 * SDL order; Player 2 gets one of them. */
int         Pc_Split_PadCount(void);
const char* Pc_Split_PadName(int k);

/* Player 1 picked Start: arrange this window, launch Player 2's with pad k,
 * their character and the room port, and stop reading that pad here. The room
 * must already be requested (ShSession_RequestHostListen). 0 and *err on
 * failure. */
int  Pc_Split_Start(int padK, int playAs, int port, const char** err);

/* Player 2's window is up but has not joined yet: the lobby shows the status
 * and the game starts itself once they are in. HostStarted clears it. */
int  Pc_Split_HostWaiting(void);
void Pc_Split_HostStarted(void);
void Pc_Split_StatusText(char* out, int cap);

/* Back to one window: restores Player 1's window mode, lets the pad be read
 * again, and closes Player 2's game. Called when the session ends. */
void Pc_Split_Stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_SPLIT_SCREEN_H */
