/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See pc_coop_save.h. Game thread only. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/memcard.h"
#include "bodyprog/savegame.h"

#include "pc_coop_save.h"
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Card image layout, as PsyCross's libapi.c writes it: 16 blocks of 8 KiB,
 * block 0 holds the directory (one 128-byte frame per file, frames 1-15) and
 * file N's data starts at block N. */
#define CARD_BLOCK      8192
#define CARD_FRAME      128
#define CARD_DIR_FILES  15
#define CARD_ATTR_FIRST 0x50

/* Inside a Silent Hill file (memcard.c): icon block, then the save headers,
 * the options, and up to 11 saves. */
#define SAVE_HEADER_OFS 0x200
#define SAVE_DATA_OFS   0x380

extern char* g_Savegame_SaveLocationNames[];

static FILE* Coop_OpenCard(int card)
{
    char path[64];
    snprintf(path, sizeof(path), "gamedata/save/%d.MCD", card);
    return fopen(path, "rb");
}

/* Port 1 slots A-D and port 2 slots A-D, numbered as libapi.c numbers them. */
static int Coop_CardAt(int i)
{
    return (i < 4) ? i : i + 4;
}

static int Coop_ReadSlot(FILE* f, int dirIdx, int slot, s_Savegame_Container* out)
{
    long ofs = (long)dirIdx * CARD_BLOCK + SAVE_DATA_OFS + (long)slot * (long)sizeof(s_Savegame_Container);
    if (fseek(f, ofs, SEEK_SET) != 0 || fread(out, sizeof(*out), 1, f) != 1)
    {
        return 0;
    }
    return MemCard_ChecksumValidate(&out->footer, (s8*)out, sizeof(*out)) ? 1 : 0;
}

/* The directory index of every Silent Hill file on a card for this region. */
static int Coop_GameFiles(FILE* f, int* out)
{
    unsigned char dir[(CARD_DIR_FILES + 1) * CARD_FRAME];
    char          prefix[32];
    size_t        plen;
    int           i, n = 0;

    MemCard_FilenameGenerate(prefix, 0);
    plen = strlen(prefix) - 2; /* the two digits are the file number */
    if (fseek(f, 0, SEEK_SET) != 0 || fread(dir, sizeof(dir), 1, f) != 1 || dir[0] != 'M' || dir[1] != 'C')
    {
        return 0;
    }
    for (i = 1; i <= CARD_DIR_FILES; i++)
    {
        const unsigned char* e = dir + i * CARD_FRAME;
        if ((e[0] & 0xF0) == CARD_ATTR_FIRST && strncmp((const char*)e + 10, prefix, plen) == 0)
        {
            out[n++] = i;
        }
    }
    return n;
}

static int Coop_NewestFirst(const void* a, const void* b)
{
    return ((const CoopSaveEntry*)b)->order - ((const CoopSaveEntry*)a)->order;
}

int Pc_CoopSave_List(CoopSaveEntry* out, int max)
{
    int c, n = 0;

    if (!out || max <= 0)
    {
        return 0;
    }
    for (c = 0; c < 8; c++)
    {
        int   files[CARD_DIR_FILES];
        int   nf, fi;
        FILE* f = Coop_OpenCard(Coop_CardAt(c));
        if (!f)
        {
            continue;
        }
        nf = Coop_GameFiles(f, files);
        for (fi = 0; fi < nf; fi++)
        {
            s_MemCard_SaveHeader hdr;
            int                  slot;
            if (fseek(f, (long)files[fi] * CARD_BLOCK + SAVE_HEADER_OFS, SEEK_SET) != 0 ||
                fread(&hdr, sizeof(hdr), 1, f) != 1)
            {
                continue;
            }
            for (slot = 0; slot < MEMCARD_SAVES_COUNT_MAX; slot++)
            {
                s_Savegame_Container sc;
                const s_Savegame*    sg = &sc.savegame;
                int                  loc, secs;
                CoopSaveEntry*       e;

                if (hdr.saveMetadata[slot].savegameCount == 0 || !Coop_ReadSlot(f, files[fi], slot, &sc))
                {
                    continue;
                }
                if (n >= max)
                {
                    /* Full: keep the newest, dropping the oldest held. */
                    qsort(out, (size_t)n, sizeof(*out), Coop_NewestFirst);
                    if (hdr.saveMetadata[slot].totalSavegameCount <= out[n - 1].order)
                    {
                        continue;
                    }
                    n--;
                }
                e    = &out[n++];
                loc  = sg->locationId;
                secs = FP_FROM(sg->gameplayTimer, Q12_SHIFT);
                if (loc < 0 || loc > SaveLocationId_NextFear) loc = SaveLocationId_Anywhere;
                snprintf(e->name, sizeof(e->name), "%s  %d:%02d:%02d  (save %d)",
                         g_Savegame_SaveLocationNames[loc], secs / 3600, (secs / 60) % 60, secs % 60,
                         (int)sg->savegameCount);
                {
                    char* p;
                    for (p = e->name; *p; p++) if (*p == '_') *p = ' ';
                }
                snprintf(e->key, sizeof(e->key), "%d.%d.%d", Coop_CardAt(c), files[fi], slot);
                e->mapIdx = sg->mapIdx;
                e->order  = hdr.saveMetadata[slot].totalSavegameCount;
            }
        }
        fclose(f);
    }
    qsort(out, (size_t)n, sizeof(*out), Coop_NewestFirst);
    return n;
}

static int Coop_ReadKey(const char* key, s_Savegame* out)
{
    int                  card, dirIdx, slot, ok;
    s_Savegame_Container sc;
    FILE*                f;

    if (!key || sscanf(key, "%d.%d.%d", &card, &dirIdx, &slot) != 3 ||
        dirIdx < 1 || dirIdx > CARD_DIR_FILES || slot < 0 || slot >= MEMCARD_SAVES_COUNT_MAX)
    {
        return 0;
    }
    f = Coop_OpenCard(card);
    if (!f)
    {
        SH_DBG("[COOPSAVE] card %d is gone", card);
        return 0;
    }
    ok = Coop_ReadSlot(f, dirIdx, slot, &sc);
    fclose(f);
    if (!ok)
    {
        SH_DBG("[COOPSAVE] save %s is damaged", key);
        return 0;
    }
    *out = sc.savegame;
    return 1;
}

int Pc_CoopSave_Load(const char* key)
{
    if (!Coop_ReadKey(key, g_SavegamePtr))
    {
        return 0;
    }
    SH_DBG("[COOPSAVE] loaded save %s (map %d)", key, (int)g_SavegamePtr->mapIdx);
    return 1;
}
