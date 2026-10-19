/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_npc.h - host-owned monsters in co-op (sh_net_npc.c).
 *
 * Hooks for npc_main.c (spawn, update, despawn), bodyprog_anim (pose
 * capture) and the positional sound functions (monster sounds).
 */
#ifndef SH_NET_NPC_H
#define SH_NET_NPC_H

enum
{
    PC_NETNPC_NONE  = 0,
    PC_NETNPC_HOST  = 1,
    PC_NETNPC_GUEST = 2
};

int  Pc_NetNpc_Role(void);

/* Host. */
void Pc_NetNpc_OnSpawn(int slot);                 /* table spawn landed in this slot */
void Pc_NetNpc_BeginUpdate(int slot, GsCOORDINATE2* bones);
void Pc_NetNpc_EndUpdate(void);
void Pc_NetNpc_CapturePose(s32 k0, s32 k1, s32 alpha);
void Pc_NetNpc_Sfx(const void* pos, int sfxId, int vol, int pitched);
extern GsCOORDINATE2* g_PcNetNpcCapBones;         /* non-NULL while a tracked AI runs */

/* Guest. */
int  Pc_NetNpc_GuestSkipRow(int row, s32 spawnX, s32 spawnZ);
int  Pc_NetNpc_IsPuppet(int slot);
void Pc_NetNpc_PuppetTick(s_SubCharacter* npc, int slot, s_AnmHeader* hdr, GsCOORDINATE2* bones);

/* Both: once a frame from ShNet_GameTick. */
void Pc_NetNpc_GameTick(void);

/* A room/map load wiped g_SysWork.npcs; a slot was freed by a despawn. */
void Pc_NetNpc_Reset(void);
void Pc_NetNpc_OnFree(int slot);
u32  Pc_NetNpc_NearestPlayerD2(const s_SubCharacter* npc, u32 d2);

/* Host: where the partners are, so chunks, collision and spawns reach them. */
int  Pc_NetNpc_PartnerCells(s32* px, s32* pz, s32* cx, s32* cz, int max);
int  Pc_NetNpc_ChunkNearPartner(s32 cellX, s32 cellZ, int isExterior);
int  Pc_NetNpc_PartnerSpawnOk(s32 x, s32 z, int cond);

/* Any member: we took a pickup (its event flag), so nobody else can. */
void Pc_NetNpc_PickupTaken(int eventFlagIdx);

#endif /* SH_NET_NPC_H */
