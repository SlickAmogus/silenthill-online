/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_POOL_H
#define PC_CHARA_POOL_H

/* Global character/asset pool (config: global_chara_pool, default on).
 * Makes every chara's model+texture+anim resident PC-side so any monster can
 * spawn in any map, and backfills AI update funcs from chara_global.dll.
 * Design doc: pc_port/docs/Global_Chara_Pool.md. */

#ifdef __cplusplus
extern "C" {
#endif

/* Load/refresh the pool. Called from the map-load state machine (game_load.c
 * case 6) once the FS queue is idle: first call loads every chara's assets
 * (~1.3 MB, one-time); later calls only reload entries whose CHARA_FILE_INFOS
 * file idxs changed (JPN per-map GreyChild/Mumbler swap, ending *_LAST anims)
 * and re-point stale registries. */
void Pc_CharaPool_OnMapLoad(void);

/* Re-point registeredCharaModels / g_CharaAnimDataIdxs at pool entries for
 * every charaId the current map does NOT have natively resident. Native
 * registrations always win. Cheap; safe to call any time outside a load. */
void Pc_CharaPool_Refresh(void);

/* Open chara_global.dll (AI update funcs for every portable monster). Own
 * DLL handle, opened once at boot, never closed. */
void Pc_CharaGlobal_Open(void);

/* Fill NULL charaUpdateFuncs slots of the active map header from
 * chara_global.dll. Called at the end of MapRegistry_Load. */
void Pc_CharaGlobal_Backfill(void);

/* True when charaId's registered model is the pool's copy (spawnable here
 * only because of the pool). */
int Pc_CharaPool_IsPoolModel(int charaId);

/* A clean hero-Harry model kept in the pool solely so REMOTE players (co-op /
 * living-world ghosts) render as vanilla Harry regardless of the local play-as
 * skin. NULL until loaded (needs global_chara_pool). */
typedef struct _CharaModel s_CharaModel;
s_CharaModel* Pc_CharaPool_HarryGhostModel(void);
/* A model to draw a remote player wearing charaId: the live native one, else
 * the pool's copy (a map script's own load can drop the registration in the
 * middle of a map). NULL when neither is resident. */
s_CharaModel* Pc_CharaPool_ModelFor(s32 charaId);

/* Per-npc-slot "debug spawn" flags: flagged slots skip the savegame
 * enemy-state bookkeeping on kill (their field_40 is an npc slot index, not
 * a spawn-table row). Set by the console SPAWN command; cleared when a
 * native spawn reuses the slot or on map load. */
extern unsigned char g_PcNpcDebugSpawned[];
void Pc_NpcDebugSpawnClearAll(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_CHARA_POOL_H */
