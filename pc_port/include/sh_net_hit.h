/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_hit.h - friendly fire: let the local player's attacks hit remote
 * players, and apply hits that remote players land on us.
 *
 * Remote players are render-only (sh_net_ghost.c). To make them HITTABLE this
 * module keeps a small array of proxy characters (one per co-op member on our
 * map, Harry's live collision cylinder at their network position) that the
 * game's own attack resolution is pointed at through four small hooks in the
 * combat code:
 *
 *   gun     func_8008A3E0 runs the bullet trace a second time against the
 *           proxies (Pc_NetHit_BulletTrace); the nearer hit takes the bullet
 *   melee   func_8008BF84's candidate loop continues past the 6 NPC slots into
 *           the proxies (Pc_NetHit_ProxyCount / Pc_NetHit_Proxy), so a player
 *           gets the exact same box-vs-cylinder and line-of-sight test
 *   latch   func_8008B714 gives a proxy a per-swing latch bit of its own
 *           (Pc_NetHit_ProxyIndex), so one swing is one hit
 *   blood   func_8005F6B0 does not register a decal for a proxy (it has no
 *           NPC index to be read back through)
 *
 * The damage roll lands in the proxy's damage fields like it would in an
 * NPC's; once a frame it is harvested (Pc_NetHit_Refresh), scaled for a
 * 100 HP target, and sent to THAT player, who applies it to their own
 * character exactly the way a monster's hit lands (Pc_NetHit_ApplyIncoming).
 * The attacker never writes anyone else's health.
 *
 * Opt-in on BOTH sides: a player advertises SHNET_PF_FFIRE while it has
 * friendly fire on; only those are proxied, and a hit that arrives anyway is
 * dropped unless this side has it on too. Nobody can be griefed by a
 * stranger's setting.
 *
 * Opaque pointers keep engine types out of this pc_port header.
 */
#ifndef SH_NET_HIT_H
#define SH_NET_HIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game thread, once per frame after the world update (ShNet_GameTick):
 * harvest and rebuild the proxies, then apply hits that landed on us. */
void  Pc_NetHit_Refresh(void);
void  Pc_NetHit_ApplyIncoming(void);

/* The proxies this frame (s_SubCharacter*), contiguous so a pointer walk
 * from Proxy(0) visits them all. */
int   Pc_NetHit_ProxyCount(void);
void* Pc_NetHit_Proxy(int i);

/* Index of a proxy character, -1 if the pointer is not one of ours. */
int   Pc_NetHit_ProxyIndex(const void* chara);
/* The resolver tells us where a hit on a proxy landed (for the victim's blood). */
void  Pc_NetHit_NoteHit(const void* chara, const VECTOR3* at);

/* Gun hook: the bullet trace against the proxies (and the world). Returns the
 * hit proxy or NULL; outTrace (s_RayTrace*) receives the hit point and
 * distance for comparison with the game's own trace. */
void* Pc_NetHit_BulletTrace(void* attacker, const void* from, void* offset, void* outTrace);

/* Victim hook: the attacker's heading for the flinch direction of a pending
 * network hit (there is no NPC to read it from). Consumes it. */
int   Pc_NetHit_TakePendingYaw(short* yaw);
/* 1 once if the damage the player is about to take came from a partner
 * (friendly fire) rather than a monster. Player_ReceiveDamage consumes it. */
int   Pc_NetHit_TakeFriendly(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_HIT_H */
