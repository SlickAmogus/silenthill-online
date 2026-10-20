/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_COOP_SCREEN_H
#define PC_COOP_SCREEN_H

/* The co-op menus drawn with the game's own primitives (pc_coop_screen.c).
 * Call once per frame while Pc_CoopMenu_IsOpen(): from the title's draw
 * (instead of the menu art) and from ShNet_GameTick in game. */
void Pc_CoopScreen_Draw(void);

#endif
