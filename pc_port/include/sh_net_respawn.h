/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_respawn.h - co-op respawn: dying in a co-op game is not game over.
 *
 * The death itself is untouched (the hit lands, the death animation plays).
 * What changes is what follows: the engine's transition into the game-over
 * sequence (fade, "Game Over", reload) is held back while a co-op game is
 * running, the body lies there for online_respawn_seconds, blinks out the way
 * old games despawn a defeated character, and the player is put back alive
 * next to a living co-op partner on the same map, or failing that at the
 * point where they entered the map.
 *
 * Game thread only.
 */
#ifndef SH_NET_RESPAWN_H
#define SH_NET_RESPAWN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once per frame after the world update (ShNet_GameTick). */
void  ShNetRespawn_Tick(void);

/* 1 from the moment the local player dies in a co-op game until they are
 * standing again. */
int   ShNetRespawn_Active(void);

/* Seconds until the player is put back, while Active. */
float ShNetRespawn_Remaining(void);

/* 1 on the frames the body is blinked out (the despawn flicker); the player
 * model draw skips those frames. */
int   ShNetRespawn_HideModel(void);

/* 1 while the engine must not enter its game-over sequence for this death. */
int   ShNetRespawn_BlocksGameOver(void);

/* Engine hooks (player_control.c). HoldDeath is called where a death handler
 * would enter SysState_GameOver: returns 1 (and starts the countdown) when the
 * co-op respawn takes the death instead, so the handler returns and the locked
 * corpse stays. Holding is 1 while that corpse must not be re-unlocked or
 * re-shown by the death handler's per-frame flag re-assert. */
int   Pc_Respawn_HoldDeath(void);
int   Pc_Respawn_Holding(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_RESPAWN_H */
