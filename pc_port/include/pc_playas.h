/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PLAYAS_H
#define PC_PLAYAS_H

/* Play as another character (config: player_character, console: PLAYAS,
 * K-view - / = cycling). Swaps the player's rendered ILM+TIM by retargeting
 * CHARA_FILE_INFOS[Chara_Harry]'s model/texture rows — the same mechanism the
 * PAL Mumbler censorship and the chara pool's beta retargets use — while
 * keeping HB_BASE.ANM, playerBoneCoords and all gameplay logic untouched, so
 * every Harry animation drives the substituted body directly. */

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the config-selected character before the boot-time
 * WorldGfx_HarryCharaLoad. Only retargets table rows; the boot load and
 * WorldGfx_PlayerModelProcessLoad do the rest. */
void Pc_PlayAs_Init(void);

int         Pc_PlayAs_Count(void);
int         Pc_PlayAs_Current(void);
const char* Pc_PlayAs_Label(int idx);   /* uppercase display name */
const char* Pc_PlayAs_Name(int idx);    /* lowercase config/console token */
int         Pc_PlayAs_SkinCharaId(void);/* Chara_Harry when not swapped */
/* The HERO_* swap worn, as its lowercase token ("cj"), or "" for a built-in
 * character. What a co-op partner is told, so they can wear the same file. */
const char* Pc_PlayAs_SkinToken(void);
/* The swap file ("HERO_CJ") this machine has for a token, or NULL. */
const char* Pc_PlayAs_SkinFileForToken(const char* token);
int         Pc_PlayAs_IsFemale(void);   /* gates the optional voice-pitch shift */

/* Select a character. Mid-game this synchronously reloads the player model
 * (held item freed + re-equipped around the disc reads). save != 0 persists
 * the choice to config.cfg. Returns 1 on success. */
int Pc_PlayAs_SetByIndex(int idx, int save);
int Pc_PlayAs_SetByName(const char* name, int save); /* case-insensitive */
int Pc_PlayAs_Cycle(int step);                       /* wraps; returns new index */

/* world_draw.c hooks: */

/* After Pc_BigLm_Redirect in WorldGfx_HarryCharaLoad. A non-HERO player ILM
 * must never land in the HERO-sized PSX slab: disc reads are sector-granular
 * (16 KiB) and overrun HELD_ITEM_LM_BUFFER, so hand out a PC-owned buffer.
 * Loose/v7 redirects (already-owned pointers) pass through untouched. */
void* Pc_PlayAs_PlayerLmRedirect(int modelFileIdx, void* lmHdr);

/* Same place, on the image desc: a skin TIM is larger than the parcel
 * Chara_FsImageCalc sizes for HERO.TIM, and the overrun lands on the chara CLUT
 * shelf below it (rainbow enemies map-wide). Route non-HERO player textures to a
 * virtual pool slot so they never touch VRAM. Must run BEFORE the queued read —
 * the same desc feeds both the upload and harryModel.texture's material bake. */
void Pc_PlayAs_PlayerImageDesc(void* imageDesc /* s_FsImageDesc* */);

/* After WorldGfx_CharaModelProcessLoad(&harryModel): hide the skin's embedded
 * prop meshes (guns/bag/key/extra hands), set up Lisa's hair coords, and
 * register HERO.TIM in a virtual texture slot for the HERO-textured weapons. */
void Pc_PlayAs_OnPlayerModelLoaded(void);

/* Repoint the held-weapon image desc (knife/hammer/axe/handgun/rifle/shotgun
 * PLMs are textured from "HERO" = Harry's VRAM parcel, which now holds the
 * skin's sheet) at the virtual-slot copy of HERO.TIM. */
void Pc_PlayAs_HeldItemImageDesc(void* imageDesc /* s_FsImageDesc* */);

/* True when the Harry hand-variant part toggles (func_8003DE60) must not run:
 * its hard-coded part indices target HERO's part order and scramble any other
 * ILM. The caller re-applies the skin visibility table instead. */
int Pc_PlayAs_SuppressHarryHandVariants(void);
void Pc_PlayAs_ApplySkinVisibility(void);

/* Per-frame from Player_Update: keep Lisa's rigid hair coords composed (their
 * GsCOORDINATE2 flg caches must clear every frame) and re-init them after a
 * warm reset's SysWork_Clear. */
void Pc_PlayAs_PlayerAnimTick(void);

/* ---- a skin's rig, for drawing OTHER players wearing it (sh_net_ghost.c).
 *
 * Everything the local play-as applies to Harry's skeleton when the player
 * wears a skin, in one record: the skin's own bind translations for the
 * static bones (posed on Harry's they stretch the neck and limbs), its root
 * height, its extra bones past Harry's 18 (hair, parasite, sleeves) with
 * their rest rotation, and the embedded parts to hide (guns, bag, the spare
 * faces). Loaded off the disc once per skin and kept. */
#define PC_PLAYAS_RIG_BONES 18
#define PC_PLAYAS_RIG_EXTRA 5
typedef struct
{
    int                valid;
    int                charaId;
    int                bindT[PC_PLAYAS_RIG_BONES][3];
    unsigned char      bindHas[PC_PLAYAS_RIG_BONES];
    unsigned char      rootY;        /* rootYOffset for Harry's keyframes on this rig */
    int                extraCount;
    int                extraT[PC_PLAYAS_RIG_EXTRA][3];
    int                extraR[PC_PLAYAS_RIG_EXTRA][9];
    unsigned char      extraParent[PC_PLAYAS_RIG_EXTRA];
    const char* const* hideParts;    /* NULL-terminated 8-char part names, or NULL */
} PcPlayAsRig;

/* Load a roster skin's rig if it is not loaded yet (a synchronous disc read,
 * like the pool's); call from the game tick, not from a draw. No-op for
 * Harry and for ids outside the roster. */
void Pc_PlayAs_RigPrefetch(int charaId);
/* The loaded rig for a roster skin, NULL for Harry / not a skin / not loaded. */
const PcPlayAsRig* Pc_PlayAs_RigFor(int charaId);
/* Harry's own rootYOffset, whatever the LOCAL skin patched into HB_BASE. */
int Pc_PlayAs_HarryRootY(void);
/* Hide the rig's embedded parts on a chara model before drawing it as this
 * skin, and put the model's own visibility back after: the model is the
 * pool's shared copy and an NPC may be wearing it too. saved[] holds 64 bits. */
void Pc_PlayAs_RigHideParts(const PcPlayAsRig* rig, void* charaModel /* s_CharaModel* */, unsigned int saved[2]);
void Pc_PlayAs_RigRestoreParts(void* charaModel /* s_CharaModel* */, const unsigned int saved[2]);

#ifdef __cplusplus
}
#endif

#endif
