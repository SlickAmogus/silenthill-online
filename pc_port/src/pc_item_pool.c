/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_item_pool.c - held-item models for OTHER players (co-op).
 *
 * The game keeps exactly one held-item model resident (g_WorldGfxWork.heldItem
 * in HELD_ITEM_LM_BUFFER, its texture in one fixed VRAM parcel), so a partner
 * could only be drawn with a weapon when it was the same one we hold. On PC the
 * weapon PLMs live in their own malloc'd buffers here, textured either from
 * Harry's HERO sheet (knife, hammer, axe, handgun, rifle, shotgun share it) or
 * from their own TIM decoded into a persistent virtual texture slot, the way
 * pc_chara_pool.c keeps every monster resident. Loaded on demand, synchronously
 * (a few KB each), and kept for the session.
 */

#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"
#include "gpu.h"

#include "hires_override.h"
#include "pc_item_pool.h"
#include "pc_playas.h"
#include "sh_log.h"

typedef struct
{
    int         itemId;
    int         plmIdx;
    int         timIdx;   /* -1: textured from HERO */
    const char* tex;
} PcItemDef;

/* The weapons a player can hold in co-op. Cutscene props are never shown on
 * a partner. */
static const PcItemDef s_defs[] = {
    { InvItemId_KitchenKnife, FILE_ITEM_KNIFE_PLM,   -1,                   "HERO"   },
    { InvItemId_SteelPipe,    FILE_ITEM_PIPE_PLM,    FILE_ITEM_PIPE_TIM,   "PIPE"   },
    { InvItemId_Hammer,       FILE_ITEM_HAMMER_PLM,  -1,                   "HERO"   },
    { InvItemId_Axe,          FILE_ITEM_AXE_PLM,     -1,                   "HERO"   },
    { InvItemId_Handgun,      FILE_ITEM_HANDGUN_PLM, -1,                   "HERO"   },
    { InvItemId_HuntingRifle, FILE_ITEM_RIFLE_PLM,   -1,                   "HERO"   },
    { InvItemId_Shotgun,      FILE_ITEM_SHOTGUN_PLM, -1,                   "HERO"   },
    { InvItemId_Chainsaw,     FILE_ITEM_CSAW_PLM,    FILE_ITEM_CSAW_TIM,   "CSAW"   },
    { InvItemId_HyperBlaster, FILE_ITEM_HPRGUN_PLM,  FILE_ITEM_HPRGUN_TIM, "HPRGUN" },
    { InvItemId_RockDrill,    FILE_ITEM_DRILL_PLM,   FILE_ITEM_DRILL_TIM,  "DRILL"  },
    { InvItemId_Katana,       FILE_ITEM_KATANA_PLM,  FILE_ITEM_KATANA_TIM, "KATANA" },
};
#define ITEM_POOL_COUNT ((int)(sizeof(s_defs) / sizeof(s_defs[0])))

/* Virtual texture slots for the own-TIM weapons: past the play-as slot (300),
 * below the chara spill sets (320+). */
#define ITEM_POOL_SLOT0 301

typedef struct
{
    s_HeldItem hi;
    u8*        buf;
    u8*        timBuf;
    int        state; /* 0 untouched, 1 ready, 2 failed */
} PcItemEntry;

static PcItemEntry s_items[ITEM_POOL_COUNT];

static int PcItem_Index(int itemId)
{
    int i;
    for (i = 0; i < ITEM_POOL_COUNT; i++)
    {
        if (s_defs[i].itemId == itemId) return i;
    }
    return -1;
}

static void PcItem_Load(int i)
{
    const PcItemDef* d = &s_defs[i];
    PcItemEntry*     e = &s_items[i];
    s_HeldItem*      hi = &e->hi;
    s32              size, pump = 0;

    e->state = 2;
    memset(hi, 0, sizeof(*hi));
    size = Fs_GetFileSectorAlignedSize(d->plmIdx);
    if (size <= 0)
    {
        return;
    }
    e->buf = (u8*)calloc(1, (size_t)size + 64);
    if (e->buf == NULL)
    {
        return;
    }
    hi->itemId      = d->itemId;
    hi->textureName = (char*)d->tex;
    hi->lmHdr       = (s_LmHeader*)e->buf;

    if (d->timIdx < 0)
    {
        /* Harry's sheet, as WorldGfx_PlayerHeldItemSet sets it. */
        hi->imageDesc.tPage[1] = 27;
        hi->imageDesc.tPage[0] = 0;
        hi->imageDesc.u        = 0;
        hi->imageDesc.v        = 0;
        hi->imageDesc.clutX    = 736;
        hi->imageDesc.clutY    = 480;
    }
    else
    {
        /* Own TIM, decoded into a virtual slot: no VRAM parcel to fight over
         * (canonical encoding in hires_override.h; Fs_QueuePostLoadTim skips
         * the VRAM uploads for a clutY past the real rows). */
        const int slot = ITEM_POOL_SLOT0 + i;
        s32       tsize = Fs_GetFileSectorAlignedSize(d->timIdx);
        if (tsize <= 0)
        {
            return;
        }
        e->timBuf = (u8*)calloc(1, (size_t)tsize + 64);
        if (e->timBuf == NULL)
        {
            return;
        }
        HiresOverride_CharaPoolSlotReset(slot);
        hi->imageDesc.tPage[0] = 0;
        hi->imageDesc.tPage[1] = 28;
        hi->imageDesc.u        = 0;
        hi->imageDesc.v        = 0;
        hi->imageDesc.clutX    = (s16)((slot % 64) * 16);
        hi->imageDesc.clutY    = (s16)(HIRES_POOL_CLUT_ROW_BASE + (slot / 64) * HIRES_POOL_MAX_ROWS);
        Fs_QueueStartReadTim(d->timIdx, e->timBuf, &hi->imageDesc);
    }
    Fs_QueueStartRead(d->plmIdx, e->buf);
    /* Synchronous, like the chara pool: reads are synchronous on PC and the
     * queue index must not be trusted across frames. */
    while (Fs_QueueGetLength() > 0 && pump++ < 100000)
    {
        Fs_QueueUpdate();
    }
    if (e->buf[0] == 0 && e->buf[1] == 0 && e->buf[2] == 0 && e->buf[3] == 0)
    {
        SH_DBG("[ITEMPOOL] item %d: model did not load", d->itemId);
        return;
    }

    /* The bake WorldGfx_HeldItemDraw does once per load. */
    {
        extern void LmHeader_FixOffsets(s_LmHeader* lmHdr);
        LmHeader_FixOffsets(hi->lmHdr);
        if (d->timIdx < 0)
        {
            /* Harry's parcel holds the local skin's sheet under play-as; the
             * hook hands out the clean HERO copy then. */
            Pc_PlayAs_HeldItemImageDesc(&hi->imageDesc);
        }
        Lm_MaterialFsImageApply1(hi->lmHdr, hi->textureName, &hi->imageDesc, BlendMode_Additive);
        Lm_MaterialFlagsApply(hi->lmHdr);
        Bone_ModelAssign(&hi->bone, hi->lmHdr, 0);
        hi->lmHdr->isLoaded = true;
    }
    e->state = 1;
    SH_DBG("[ITEMPOOL] item %d (%s) resident for partners", d->itemId, d->tex);
}

void Pc_ItemPool_Want(int itemId)
{
    int i = PcItem_Index(itemId);
    if (i < 0 || s_items[i].state != 0)
    {
        return;
    }
    if (g_GameWork.gameState != GameState_InGame)
    {
        return; /* not during a load: the queue is the map's then */
    }
    PcItem_Load(i);
}

s_HeldItem* Pc_ItemPool_Get(int itemId)
{
    int i = PcItem_Index(itemId);
    if (i < 0 || s_items[i].state != 1)
    {
        return NULL;
    }
    return &s_items[i].hi;
}
