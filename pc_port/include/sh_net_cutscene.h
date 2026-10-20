/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SH_NET_CUTSCENE_H
#define SH_NET_CUTSCENE_H

/* One cutscene for everyone in co-op (sh_net_cutscene.c). */

#ifdef __cplusplus
extern "C" {
#endif

/* game_sys_states.c: right after SysWork_StateSetNext(g_MapEventSysState). */
void ShNetCut_OnDispatch(void);
/* game_sys_states.c: before dispatching g_MapEventSysState. Returns 1 (and
 * cancels this frame's dispatch) while a map-entry scene waits for a partner
 * who is still loading. */
int  ShNetCut_HoldDispatch(void);
/* Once a frame from ShNet_GameTick. */
void ShNetCut_Tick(void);
/* SHNET_CTRL_CUTSCENE / SHNET_CTRL_DIE from the room (sh_net_game.c hands them over). */
void ShNetCut_CtrlIn(unsigned long long from, const unsigned char* p, int len);
void ShNetCut_DieIn(unsigned long long from, const unsigned char* p, int len);
/* player_control.c: the alley death (map0_s00) just ended; everyone dies with us. */
void ShNetCut_OnSharedDeath(void);
/* We are in a scene (co-op, in game). */
int  ShNetCut_InScene(void);
/* Where a partner stands in a shared scene relative to us: 0 = our spot,
 * +1 one step to the right, -1 one to the left, and so on. Stable across
 * every player's screen (ordered by id). */
int  ShNetCut_SceneSlot(unsigned long long id);
/* A line for the screen while a hold or a join is pending. */
int  ShNetCut_StatusText(char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif
