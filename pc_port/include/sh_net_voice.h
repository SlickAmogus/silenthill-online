/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SH_NET_VOICE_H
#define SH_NET_VOICE_H

/* The vocalize wheel: hold a key, pick one of eight lines of your character,
 * everyone on the map hears it (sh_net_voice.c). */

#ifdef __cplusplus
extern "C" {
#endif

/* dbg_overlay.c: the vocalize key's state this frame (0 while a text field
 * has the keyboard). Opens the wheel on press, says the line on release. */
void        ShNetVoice_KeyHeld(int held);
/* Once a frame from ShNet_GameTick. */
void        ShNetVoice_Tick(void);
/* SHNET_CTRL_VOICE from the room. */
void        ShNetVoice_CtrlIn(unsigned long long from, const unsigned char* p, int len);

/* For the overlay. */
int         ShNetVoice_MenuOpen(void);
int         ShNetVoice_MenuSelected(void);    /* -1 none */
const char* ShNetVoice_MenuLabel(int slot);   /* 0..7 */

#ifdef __cplusplus
}
#endif

#endif
