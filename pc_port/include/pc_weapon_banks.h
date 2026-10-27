/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_WEAPON_BANKS_H
#define PC_WEAPON_BANKS_H

/* Every weapon sound bank resident at once (pc_weapon_banks.c), so a partner's
 * weapon can be heard whatever we hold. */

#ifdef __cplusplus
extern "C" {
#endif

/* The libsd vab slot holding the bank SD_Call `sdCall` (163..169) loads, read
 * off the disc the first time; -1 when it cannot be had. */
int Pc_WeaponBank_VabId(int sdCall);

/* sd_call.c: while non-zero, a weapon sound (AudioType_Weapon) keys on this
 * vab slot instead of the loaded weapon bank. */
extern int g_PcSfxVabOverride;

#ifdef __cplusplus
}
#endif

#endif
