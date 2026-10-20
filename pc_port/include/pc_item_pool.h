/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ITEM_POOL_H
#define PC_ITEM_POOL_H

/* Held-item models for other players (pc_item_pool.c): every weapon a
 * partner can hold, resident in PC memory, so a partner is drawn with the
 * weapon they actually hold rather than only when it matches ours. */

#ifdef __cplusplus
extern "C" {
#endif

/* Make this weapon resident (synchronous disc read the first time; a no-op
 * after). Call from the game tick, not from a draw. */
void Pc_ItemPool_Want(int itemId);

/* The baked model for a weapon, NULL until loaded (or not a weapon). The
 * returned s_HeldItem's bone.modelInfo draws with func_80057090 like the
 * player's own held item. */
struct _HeldItem* Pc_ItemPool_Get(int itemId);

#ifdef __cplusplus
}
#endif

#endif
