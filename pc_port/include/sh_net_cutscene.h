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
 * cancels this frame's dispatch) while a map-entry scene waits for partners,
 * or a story door waits for the partners who have not been through it. */
int  ShNetCut_HoldDispatch(void);
/* game_sys_states.c: before the dispatch test. A story door whose wait is
 * over is dispatched from here, as if its trigger had just fired. */
void ShNetCut_GateInject(void);
/* Once a frame from ShNet_GameTick. */
void ShNetCut_Tick(void);
/* SHNET_CTRL_CUTSCENE / _DIE / _GATE from the room (sh_net_game.c hands them over). */
void ShNetCut_CtrlIn(unsigned long long from, const unsigned char* p, int len);
void ShNetCut_DieIn(unsigned long long from, const unsigned char* p, int len);
void ShNetCut_GateIn(unsigned long long from, const unsigned char* p, int len);
/* player_control.c: the alley death (map0_s00) just ended; everyone dies with us. */
void ShNetCut_OnSharedDeath(void);
/* We are in a scene (co-op, in game). */
int  ShNetCut_InScene(void);
/* Our seat in the scene we are in: the offset (Q19.12) and turn (Q3.12) to
 * draw and report ourselves at. 0 when there is none. */
int  ShNetCut_SelfOffset(int* ox, int* oz, int* turn);
/* player_control.c: applies that seat to the model's root, after the
 * position copy. The simulated position is untouched. */
void ShNetCut_SelfOffsetApply(void* coords); /* GsCOORDINATE2* */
/* A line for the screen while a hold, a door wait or a join is pending. */
int  ShNetCut_StatusText(char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif
