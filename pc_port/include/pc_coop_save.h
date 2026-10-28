/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_save.h - starting a multiplayer game from a single-player save.
 *
 * Multiplayer has no saves of its own. Every player keeps their normal
 * memory-card saves (gamedata/save/N.MCD). The host starts a game from any of
 * them; guests boot from the host's savegame (sent with the world) so one
 * playthrough is shared, and everyone saves at save points into their own
 * cards like a single-player game. These read the card images directly so
 * the multiplayer menu can list and boot a save without the load screen.
 */
#ifndef PC_COOP_SAVE_H
#define PC_COOP_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_SAVE_NAME_MAX 48
#define COOP_SAVE_KEY_MAX  16

typedef struct
{
    char name[COOP_SAVE_NAME_MAX]; /* "Cafe  1:23  (save 4)", for the menu */
    char key[COOP_SAVE_KEY_MAX];   /* card.file.slot, for Pc_CoopSave_Load */
    int  mapIdx;
    int  order;                    /* the game's running save counter; newest is highest */
} CoopSaveEntry;

/* Fill `out` with up to `max` single-player saves, newest first. Returns the
 * count. */
int Pc_CoopSave_List(CoopSaveEntry* out, int max);

/* Read the save named by `key` into g_SavegamePtr. Returns 1 on a valid save. */
int Pc_CoopSave_Load(const char* key);

/* Open the game's own save screen, as a save point does. Returns 0 (and
 * opens nothing) during a boss fight or outside normal play. Defined in
 * pc_quicksave.c. */
int Pc_Save_OpenSaveScreen(void);

/* 1 while a boss is alive on the map: saving is refused there, same as the
 * original game's lack of a save point in a boss chamber. Defined in
 * pc_quicksave.c. */
int Pc_Save_BossActive(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_COOP_SAVE_H */
