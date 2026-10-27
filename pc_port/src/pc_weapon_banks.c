/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_weapon_banks.c - every weapon sound bank resident at once (co-op).
 *
 * A weapon sound (sfx 1283..1303, AudioType_Weapon) keys on whatever VAB sits
 * in vab slot 1, which the game reloads for the weapon the LOCAL player
 * equips (func_8007F14C, SD_Call 163..169). So a partner's knife swish played
 * over our handgun bank came out as a gunshot. The PSX SPU map is full
 * (512 KiB, reverb at the top), but PsyCross backs 2 MiB: the seven weapon
 * VABs are read off the disc once and opened in libsd slots 4..10 above the
 * PSX map, and a partner's weapon sound keys on its own bank through
 * g_PcSfxVabOverride (sd_call.c).
 */

#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/libsd.h"
#include "bodyprog/sound/sound_system.h"
#include "main/fileinfo.h"

#include "lang_text.h" /* Pc_LangReadDiscFile */
#include "pc_weapon_banks.h"
#include "sh_log.h"

#define WB_FIRST_CALL 163
#define WB_COUNT      7
#define WB_FIRST_VAB  4          /* libsd vab_h slots; the game uses 0..3 */
#define WB_SPU_BASE   0x80000u   /* first byte past the PSX SPU map */
#define WB_SPU_STRIDE 0x8000u    /* bodies are <= 24528 bytes */

static unsigned char* s_data[WB_COUNT];
static int            s_vab[WB_COUNT];
static int            s_tried[WB_COUNT];

extern s16 SdVabOpenHeadStickyPc(u8* addr, s16 vabid, u32 sbaddr); /* smf_snd.c */

static int WB_Load(int k)
{
    s_AudioItemData* row = &g_AudioData[WB_FIRST_CALL - 160 + k];
    int              j, file = -1;
    unsigned int     size;
    s16              vab;
    extern s_AudioItemData* g_Sd_VabTargetLoad;

    s_tried[k] = 1;
    s_vab[k]   = -1;
    if (row->typeIdx_0 != AudioType_Weapon)
    {
        return -1;
    }
    /* fileOffset_8 is the bank's start sector (fileinfo.c repoints it per
     * disc); the table gives the file by the same sector. */
    for (j = 0; j < FS_FILE_COUNT; j++)
    {
        if ((s32)g_FileTable[j].startSector == row->fileOffset_8)
        {
            file = j;
            break;
        }
    }
    if (file < 0)
    {
        SH_DBG("[WEPBANK] bank %d: no file at sector %d", k, row->fileOffset_8);
        return -1;
    }
    size = (unsigned int)Fs_GetFileSectorAlignedSize(file);
    if (size < row->vagDataOffset_2 + 16u)
    {
        return -1;
    }
    s_data[k] = Pc_LangReadDiscFile((unsigned int)row->fileOffset_8, size);
    if (s_data[k] == NULL)
    {
        SH_DBG("[WEPBANK] bank %d: disc read failed", k);
        return -1;
    }
    vab = SdVabOpenHeadStickyPc(s_data[k], (s16)(WB_FIRST_VAB + k), WB_SPU_BASE + (unsigned int)k * WB_SPU_STRIDE);
    if (vab < 0)
    {
        SH_DBG("[WEPBANK] bank %d: header rejected", k);
        return -1;
    }
    /* The body transfer binds loose sound replacements by disc sector; it
     * reads that off the load-in-progress pointer, which nothing else holds
     * outside a bank load. */
    g_Sd_VabTargetLoad = row;
    if (SdVabTransBody(s_data[k] + row->vagDataOffset_2, vab) < 0)
    {
        g_Sd_VabTargetLoad = NULL;
        SH_DBG("[WEPBANK] bank %d: body transfer failed", k);
        return -1;
    }
    g_Sd_VabTargetLoad = NULL;
    s_vab[k] = vab;
    SH_DBG("[WEPBANK] weapon bank %d (SD_Call %d) resident as vab %d at 0x%x", k, WB_FIRST_CALL + k, vab,
           WB_SPU_BASE + (unsigned int)k * WB_SPU_STRIDE);
    return vab;
}

int Pc_WeaponBank_VabId(int sdCall)
{
    int k = sdCall - WB_FIRST_CALL;
    if (k < 0 || k >= WB_COUNT)
    {
        return -1;
    }
    if (!s_tried[k])
    {
        return WB_Load(k);
    }
    return s_vab[k];
}
