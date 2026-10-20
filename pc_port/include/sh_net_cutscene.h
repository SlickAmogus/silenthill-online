/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SH_NET_CUTSCENE_H
#define SH_NET_CUTSCENE_H

/* One cutscene for everyone in co-op (sh_net_cutscene.c). */

#ifdef __cplusplus
extern "C" {
#endif

/* game_sys_states.c: right after SysWork_StateSetNext(g_MapEventSysState). */
void ShNetCut_OnDispatch(void);
/* Once a frame from ShNet_GameTick. */
void ShNetCut_Tick(void);
/* SHNET_CTRL_CUTSCENE from the room (sh_net_game.c hands it over). */
void ShNetCut_CtrlIn(unsigned long long from, const unsigned char* p, int len);
/* The question on screen, when one is up. */
int  ShNetCut_PromptText(char* out, int cap, int* secondsLeft);

#ifdef __cplusplus
}
#endif

#endif
