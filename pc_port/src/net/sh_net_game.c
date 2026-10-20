/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_game.c - the half of the online client that knows what g_SysWork is.
 *
 * sh_net_client.c is deliberately engine-free; this file is the only place the
 * network layer reads the game. It runs once a frame from MainLoop and does
 * three things: publish where this player is, pull the worker's findings into
 * the game-thread copy, and notice the two events worth telling the server
 * about (a map change and a death).
 *
 * Everything here is a READ of game state except the death edge, which owns a
 * single static of its own. Nothing in this file can change how the game plays
 * — that is the point, and it is what keeps the online branch from drifting
 * away from the single-player one.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "bodyprog/screen/cutscene_border.h" /* CutsceneBorderState_None for the flag test */
#include "bodyprog/sound/sound_system.h"     /* Sd_PlaySfx: lobby join chime */
#include "sh_net_hit.h"                      /* friendly fire proxies + incoming hits */
#include "sh_net_respawn.h"                  /* co-op death -> respawn instead of game over */
#include "sh_net_npc.h"                      /* co-op: host-owned monsters */
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/items.h"        /* INV_ITEM_COUNT_MAX, s_InventoryItem */
#include "bodyprog/item_screens.h" /* Inventory_AddSpecialItem: grant a shared pickup */
#include "bodyprog/collision/collision.h" /* Collision_SurfaceGet: ground-snap a joining guest */
#include "sh_net_steam.h"          /* ShSteam_LobbyOwner: identify the host member */

#include "sh_net.h"
#include "sh_net_memo.h"
#include "sh_net_chat.h"
#include "sh_net_session.h"
#include "sh_net_coop.h"     /* ShNet_SetCoopActive: block pausing in a session */
#include "sh_net_internal.h"
#include "sh_net_platform.h" /* ShNetPlat_Millis, for the roster re-request throttle */
#include "pc_discord.h" /* Pc_MapAreaName, for Steam rich presence */
#include "pc_config.h"
#include "pc_playas.h"
#include "pc_item_pool.h"
#include "pc_coop_menu.h"
#include "pc_coop_screen.h"
#include "sh_net_cutscene.h"
#include "sh_net_voice.h"
#include "pc_weapon_banks.h"
#include "sh_log.h"

/* Above this the player reads as "moving fast" to a watching ghost. Q19.12,
 * and comfortably under Harry's run speed so a jog still counts. */
#define SHNET_RUN_SPEED Q12(0.035f)

static int s_lastMap      = -1;
static int s_deathLatched = 0;
static int s_initDone     = 0;

/* Throttle for the "a ghost is still unnamed" roster re-request below. */
#define SHNET_NAME_REQ_MS 3000u
static unsigned int s_lastNameReqMs = 0;

/* Recompute every frame rather than caching: the player can change character
 * mid-session (PLAYAS), and a ghost wearing the wrong body is worse than the
 * three instructions this costs. */
static int ShNetG_LocalChara(void)
{
    int id = Pc_PlayAs_SkinCharaId();
    if (id < 0 || id > 255)
    {
        id = 0;
    }
    return id;
}

static int ShNetG_BuildFlags(void)
{
    int flags = 0;

    if (g_SysWork.playerWork.player.health > Q12(0.0f))
    {
        flags |= SHNET_PF_ALIVE;
    }
    if (g_SysWork.field_2388.isFlashlightOn_15)
    {
        flags |= SHNET_PF_FLASHLIGHT;
    }
    if (g_SysWork.playerWork.player.moveSpeed > SHNET_RUN_SPEED)
    {
        flags |= SHNET_PF_RUNNING;
    }
    /* "A scene is playing." The steady no-cutscene value of cutsceneBorderState
     * is None (1), NOT 0 (0 is a one-frame teardown transient), so the old
     * `!= 0` test marked EVERY gameplay frame as a cutscene. Use the engine's
     * own idiom (game_sys_states.c). */
    if ((g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
        g_SysWork.cutsceneBorderState != CutsceneBorderState_None)
    {
        flags |= SHNET_PF_CUTSCENE;
    }
    if (g_SysWork.sysState != SysState_Gameplay)
    {
        flags |= SHNET_PF_MENU;
    }
    if (ShNet_CoopFriendlyFire())
    {
        flags |= SHNET_PF_FFIRE; /* others may shoot me; I would apply it */
    }
    if (ShSession_Active() && ShSession_IsHost() && g_PcConfig.onlineGuestConsole)
    {
        flags |= SHNET_PF_GCONSOLE;
    }
    return flags;
}

/* Co-op shared items. Each frame in a session we diff the inventory against last
 * frame's per-id totals: a count that went UP is a pickup, which we hand to the
 * other players; what they hand us we add to our own inventory. Receives are
 * folded into the snapshot before the diff, so a granted item is never bounced
 * back. Keyed by item id and summed across slots, so a merged stack and a new
 * slot both read as the same +N. Inert outside a co-op game: the snapshot resets,
 * so single-player never queues a thing. Known gap: the WORLD pickup is not yet
 * removed on the other clients, so two players walking over the same box both
 * collect it; flag-based specials (plates, stone) already de-dup themselves. */
/* What a partner's pickup may add to OUR inventory: real items only (no
 * cutscene props, no ids the item tables do not cover), at most what one
 * pickup could carry. Returns the count to grant, 0 to refuse. */
static int ShNetG_ShareableCount(int id, int cnt)
{
    int i, have = 0, used = 0;

    if (!((id >= InvItemId_HealthDrink && id <= InvItemId_Ampoule) ||
          (id >= InvItemId_LobbyKey && id <= InvItemId_ChannelingStone && id != 86) ||
          (id >= InvItemId_Chemical && id <= InvItemId_DiskOfOuroboros && id != 111) ||
          (id >= InvItemId_KitchenKnife && id <= InvItemId_Axe && id != 131) ||
          (id >= InvItemId_Handgun && id <= InvItemId_HyperBlaster) ||
          (id >= InvItemId_HandgunBullets && id <= InvItemId_ShotgunShells) ||
          (id >= InvItemId_Flashlight && id <= InvItemId_GasolineTank)))
    {
        return 0;
    }
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == id) have = 1;
        if (g_SavegamePtr->items[i].id_0 != InvItemId_Unequipped && g_SavegamePtr->items[i].id_0 != 0xFF) used++;
    }
    /* Inventory_AddSpecialItem appends at inventorySlotCount; a full bag
     * would write past items[]. */
    if (!have && (used >= INV_ITEM_COUNT_MAX - 1 || g_SavegamePtr->inventorySlotCount >= INV_ITEM_COUNT_MAX))
    {
        return 0;
    }
    if (id >= InvItemId_HandgunBullets && id <= InvItemId_ShotgunShells)
    {
        return cnt > 200 ? 200 : cnt;
    }
    if (id >= InvItemId_HealthDrink && id <= InvItemId_Ampoule)
    {
        return cnt > 10 ? 10 : cnt;
    }
    if (id >= InvItemId_Handgun && id <= InvItemId_HyperBlaster)
    {
        return have ? 0 : (cnt > 100 ? 100 : cnt);
    }
    /* Keys, puzzle items, melee weapons, tools: one, and only if missing. */
    return have ? 0 : 1;
}

static void ShNetG_SyncItems(void)
{
    extern int            g_PcCoopGame;
    static unsigned short s_snap[256];
    static int            s_snapInit;
    unsigned short        cur[256];
    int                   i, id, cnt;

    if (!g_PcCoopGame || !ShSession_Active())
    {
        s_snapInit = 0;
        return;
    }

    while (ShSession_TakeItem(&id, &cnt))
    {
        if (id > 0 && id < 256 && id != 0xFF && cnt > 0)
        {
            int t;
            cnt = ShNetG_ShareableCount(id, cnt);
            if (cnt <= 0)
            {
                continue;
            }
            Inventory_AddSpecialItem((u8)id, (u8)cnt);
            t          = (int)s_snap[id] + cnt;
            s_snap[id] = (unsigned short)(t > 65535 ? 65535 : t);
            SH_DBG("[COOP] got shared item %d x%d", id, cnt);
        }
    }

    memset(cur, 0, sizeof(cur));
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        int sid = g_SavegamePtr->items[i].id_0;
        int sc  = g_SavegamePtr->items[i].count_1;
        if (sid <= 0 || sid >= 0xFF)
        {
            continue;
        }
        {
            int t    = (int)cur[sid] + sc;
            cur[sid] = (unsigned short)(t > 65535 ? 65535 : t);
        }
    }

    if (s_snapInit)
    {
        for (i = 1; i < 0xFF; i++)
        {
            if (cur[i] > s_snap[i])
            {
                ShSession_QueueItem(i, (int)cur[i] - (int)s_snap[i]);
                SH_DBG("[COOP] shared our pickup item %d x%d", i, (int)cur[i] - (int)s_snap[i]);
            }
        }
    }
    memcpy(s_snap, cur, sizeof(s_snap));
    s_snapInit = 1;
}

/* Set by title.c when a guest boots into the host's map (join or join-in-
 * progress); cleared once we have placed him next to the host. */
int g_PcCoopGuestSpawn = 0;

/* Guest join placement. A guest who boots into the host's map should appear next
 * to the host, not at the map's New Game spawn (which, for a mid-game joiner,
 * could be a whole map away). We wait until we have actually received the host's
 * pose on this map, then drop the player there with a ground snap so he lands on
 * the floor rather than in the air or under it. The host is standing in a valid
 * spot, so placing the guest at his position cannot be inside a wall. Runs once.
 */
/* Positional sounds the local Harry made, newest last. 3d_audio.c reports
 * every func_8005DC3C / func_8005DD44 aimed at the player's own position. */
/* 3d_audio.c; no header carries them. */
void func_8005DC1C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType);
void func_8005DD44(e_SfxId sfxId, VECTOR3* pos, q23_8 vol, s8 pitch);

#define SFX_LOG     16
#define SFX_RESEND  150  /* ms a sound keeps riding outgoing poses */

static struct { unsigned int ms; unsigned short sfx; unsigned char vol, kind, seq; } s_sfxLog[SFX_LOG];
static int           s_sfxLogN;
static unsigned char s_sfxSeq;

void ShNet_LocalPlayerSfx(int sfxId, int vol, int soundType, int pitched)
{
    extern int g_PcCoopGame;
    extern int g_PcNetNpcSwapActive;
    /* During a monster's update against a guest's proxy, "the player's
     * position" is the guest's: those sounds are not ours to send. */
    if (!g_PcCoopGame || !ShSession_Active() || g_GameWork.gameState != GameState_InGame ||
        g_PcNetNpcSwapActive)
    {
        return;
    }
    if (s_sfxLogN == SFX_LOG)
    {
        memmove(&s_sfxLog[0], &s_sfxLog[1], sizeof(s_sfxLog[0]) * (SFX_LOG - 1));
        s_sfxLogN--;
    }
    s_sfxLog[s_sfxLogN].ms   = ShNetPlat_Millis();
    s_sfxLog[s_sfxLogN].sfx  = (unsigned short)sfxId;
    s_sfxLog[s_sfxLogN].vol  = (unsigned char)(vol < 0 ? 0 : vol > 255 ? 255 : vol);
    /* bit 0 = pitched path; bit 1 = played without falloff here (a sound
     * the game meant at full level for the player himself). */
    s_sfxLog[s_sfxLogN].kind = (unsigned char)((pitched ? 1 : 0) | ((soundType & 2) ? 2 : 0));
    s_sfxLog[s_sfxLogN].seq  = ++s_sfxSeq;
    s_sfxLogN++;
}

static void ShNetG_SfxAttach(ShSessionPose* pose)
{
    int i, n = 0;
    for (i = s_sfxLogN - 1; i >= 0 && n < SHSESSION_POSE_SFX; i--)
    {
        unsigned int age = pose->ms - s_sfxLog[i].ms;
        if (age > SFX_RESEND)
        {
            break;
        }
        pose->sfx[n].seq  = s_sfxLog[i].seq;
        pose->sfx[n].sfx  = s_sfxLog[i].sfx;
        pose->sfx[n].vol  = s_sfxLog[i].vol;
        pose->sfx[n].kind = s_sfxLog[i].kind;
        pose->sfx[n].age4 = (unsigned char)(age / 4u);
        n++;
    }
    /* Oldest first, so the receiver's seq check walks forward. */
    for (i = 0; i < n / 2; i++)
    {
        ShSessionSfx t = pose->sfx[i];
        pose->sfx[i] = pose->sfx[n - 1 - i];
        pose->sfx[n - 1 - i] = t;
    }
    pose->sfxCount = (unsigned char)n;
}

/* The weapon sound bank func_8007F14C loads for a weapon: one VAB of
 * AudioType_Weapon sounds per weapon family, swapped in on equip. */
static int ShNetG_WeaponSoundBank(int weapon)
{
    switch (weapon)
    {
        case EquippedWeaponId_KitchenKnife:
        case EquippedWeaponId_SteelPipe:
        case EquippedWeaponId_Hammer:
        case EquippedWeaponId_Katana:
        case EquippedWeaponId_Axe:          return 164;
        case EquippedWeaponId_Chainsaw:     return 169;
        case EquippedWeaponId_RockDrill:    return 163;
        case EquippedWeaponId_Handgun:      return 166;
        case EquippedWeaponId_HuntingRifle: return 167;
        case EquippedWeaponId_Shotgun:      return 168;
        case EquippedWeaponId_HyperBlaster: return 165;
        default:                            return -1;
    }
}

/* A weapon sound plays a slot of whichever weapon bank WE have loaded, so a
 * partner's knife swish played over our handgun bank came out as a gunshot.
 * Only play it when both banks are the same family; silence beats a wrong
 * sample. */
static int ShNetG_PartnerSfxPlayable(int sfx, int partnerWeapon)
{
    int idx = sfx - Sfx_Base;
    if (idx <= 0 || idx >= (int)(sizeof(g_Vab_InfoTable) / sizeof(g_Vab_InfoTable[0])))
    {
        return 0;
    }
    if ((g_Vab_InfoTable[idx].vab_progIdx_2 >> 8) != AudioType_Weapon)
    {
        return 1;
    }
    return ShNetG_WeaponSoundBank(partnerWeapon) >= 0 &&
           ShNetG_WeaponSoundBank(partnerWeapon) == ShNetG_WeaponSoundBank(g_Player_LastWeaponSelected);
}

/* Partners' sounds, at the partner's drawn position on the same delayed clock
 * as their body, so a footstep lands with the foot. */
static void ShNetG_PlayPartnerSfx(int myMap)
{
    static struct { unsigned long long id; unsigned int serial; } s_heard[SHSESSION_MAX_MEMBERS];
    int i, n = ShSession_MemberCount();

    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        ShSessionPoseNow       pn;
        ShSessionSfxEv         ev[8];
        int                    slot, k, got;

        if (!m || m->steamId == ShSession_SelfId() || m->mapIdx != myMap)
        {
            continue;
        }
        for (slot = 0; slot < SHSESSION_MAX_MEMBERS && s_heard[slot].id != m->steamId; slot++) {}
        if (slot == SHSESSION_MAX_MEMBERS)
        {
            /* First time we hear this member: start from now, not from a
             * backlog of sounds made before we were listening. */
            for (slot = 0; slot < SHSESSION_MAX_MEMBERS && s_heard[slot].id != 0; slot++) {}
            if (slot == SHSESSION_MAX_MEMBERS) slot = i % SHSESSION_MAX_MEMBERS;
            s_heard[slot].id     = m->steamId;
            s_heard[slot].serial = m->sfxNextSerial;
            continue;
        }
        got = ShSession_MemberTakeSfx(i, &s_heard[slot].serial, ev, 8);
        if (got <= 0 || !ShSession_MemberPoseNow(i, &pn))
        {
            continue;
        }
        for (k = 0; k < got; k++)
        {
            VECTOR3 at;
            int     idx = ev[k].sfx - Sfx_Base;
            if (idx <= 0 || idx >= (int)(sizeof(g_Vab_InfoTable) / sizeof(g_Vab_InfoTable[0])))
            {
                continue;
            }
            /* A weapon sound plays from the partner's weapon bank. When it is
             * not the one we have loaded, the resident copy of theirs is keyed
             * on instead (pc_weapon_banks.c); silence only if that bank could
             * not be had. */
            g_PcSfxVabOverride = 0;
            if ((g_Vab_InfoTable[idx].vab_progIdx_2 >> 8) == AudioType_Weapon &&
                !ShNetG_PartnerSfxPlayable(ev[k].sfx, pn.weaponAnm))
            {
                int bank = ShNetG_WeaponSoundBank(pn.weaponAnm);
                int vab  = (bank >= 0) ? Pc_WeaponBank_VabId(bank) : -1;
                if (vab < 0)
                {
                    continue;
                }
                g_PcSfxVabOverride = vab;
            }
            at.vx = pn.x; at.vy = pn.y; at.vz = pn.z;
            /* Always with distance falloff and stereo placement: a sound the
             * sender heard at full level is still out where the partner is. */
            if (ev[k].kind & 1)
            {
                func_8005DD44((e_SfxId)ev[k].sfx, &at, ev[k].vol, 0);
            }
            else
            {
                func_8005DC1C((e_SfxId)ev[k].sfx, &at, ev[k].vol, 0);
            }
            g_PcSfxVabOverride = 0;
        }
    }
}

/* Our pose for the co-op members, exactly as this frame posed us. */
static void ShNetG_PublishPose(int mapIdx, int flags)
{
    extern s32    g_PcHarryHandBone;
    extern s32    g_PcHarryPoseKf[2][3];
    ShSessionPose pose;
    s32           held = WorldGfx_HeldItemIdGet();
    int           h;

    memset(&pose, 0, sizeof(pose));
    pose.ms        = ShNetPlat_Millis();
    for (h = 0; h < 2; h++)
    {
        pose.kf[h][0] = (unsigned short)g_PcHarryPoseKf[h][0];
        pose.kf[h][1] = (unsigned short)g_PcHarryPoseKf[h][1];
        pose.alpha[h] = (unsigned short)g_PcHarryPoseKf[h][2];
    }
    ShNetG_SfxAttach(&pose);
    pose.x         = (int)g_SysWork.playerWork.player.position.vx;
    pose.y         = (int)g_SysWork.playerWork.player.position.vy;
    pose.z         = (int)g_SysWork.playerWork.player.position.vz;
    pose.rotY      = (short)g_SysWork.playerWork.player.rotation.vy;
    pose.anim      = (unsigned short)g_SysWork.playerWork.player.model.anim.status;
    pose.frame     = (unsigned short)g_SysWork.playerWork.player.model.anim.keyframeIdx;
    pose.weaponAnm = (g_Player_LastWeaponSelected >= 0 && g_Player_LastWeaponSelected < 0xFF)
                     ? (unsigned char)g_Player_LastWeaponSelected : 0xFF;
    pose.heldItem  = (held >= 0 && held < 0xFF) ? (unsigned char)held : 0xFF;
    pose.handBone  = (unsigned char)g_PcHarryHandBone;
    pose.paperMap  = (unsigned char)(g_SavegamePtr->paperMapIdx + 1);
    {
        const s_SubCharacter* me = &g_SysWork.playerWork.player;
        int st = g_SysWork.playerWork.extra.state;
        pose.pstate  = (unsigned char)((st >= 0 && st < 255) ? st : 0);
        pose.atkRecv = (me->attackReceived >= 0) ? (unsigned char)me->attackReceived : 0xFF;
        pose.cflags  = (unsigned char)(me->flags & 0xFF);
        {
            int hp = (me->health > 0) ? (int)(me->health >> 12) : 0;
            pose.health = (unsigned char)((hp > 100) ? 100 : hp);
        }
    }
    ShSession_PublishLocalPos(mapIdx, ShNetG_LocalChara(), flags, &pose);
}

/* ------------------------------------------------------------------ */
/* Co-op control notes: pause, permissions                             */
/* ------------------------------------------------------------------ */

int g_PcCoopHostPaused = 0; /* guest: the host is on the pause screen */

/* Per-guest grants the host hands out (Players list), mirrored to everyone. */
static struct { unsigned long long id; unsigned char flags; } s_perm[SHSESSION_MAX_MEMBERS];
static int          s_permN;
static unsigned int s_permSentMs;
static int          s_permDirty;

void ShNet_CoopPermSet(unsigned long long id, int flags)
{
    int i;
    for (i = 0; i < s_permN; i++)
    {
        if (s_perm[i].id == id) { s_perm[i].flags = (unsigned char)flags; s_permDirty = 1; return; }
    }
    if (s_permN < SHSESSION_MAX_MEMBERS)
    {
        s_perm[s_permN].id    = id;
        s_perm[s_permN].flags = (unsigned char)flags;
        s_permN++;
        s_permDirty = 1;
    }
}

int ShNet_CoopPermGet(unsigned long long id)
{
    int i;
    for (i = 0; i < s_permN; i++)
    {
        if (s_perm[i].id == id) return s_perm[i].flags;
    }
    return 0;
}

int ShNet_CoopPermMine(void)
{
    if (!ShSession_Active()) return 0;
    if (ShSession_IsHost()) return SHNET_PERM_CHEATS;
    return ShNet_CoopPermGet(ShSession_SelfId());
}

/* Cheats (quick-menu cheat pages, console, binds): the host always, a guest
 * when the host allowed all guests or this one. Outside co-op: always. */
int ShNet_CoopCheatsAllowed(void)
{
    extern int g_PcCoopGame;
    if (!g_PcCoopGame || !ShSession_Active() || ShSession_IsHost()) return 1;
    return ShNet_CoopGuestConsole() || (ShNet_CoopPermMine() & SHNET_PERM_CHEATS);
}

static const char* ShNetG_MemberName(unsigned long long id)
{
    int i, n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == id) return m->name;
    }
    return "A player";
}

/* Guest: pulled into the pause screen while the host is paused. */
int ShNet_CoopPauseEnter(void)
{
    extern int g_PcCoopGame;
    return g_PcCoopGame && ShSession_Active() && !ShSession_IsHost() && g_PcCoopHostPaused &&
           g_GameWork.gameState == GameState_InGame;
}

/* Guest: ejected from the pause screen when the host leaves it. */
int ShNet_CoopPauseEject(void)
{
    extern int g_PcCoopGame;
    return g_PcCoopGame && ShSession_Active() && !ShSession_IsHost() && !g_PcCoopHostPaused;
}

/* Guest: own pause press does nothing while the host holds the pause. */
int ShNet_CoopPauseHold(void)
{
    extern int g_PcCoopGame;
    return g_PcCoopGame && ShSession_Active() && !ShSession_IsHost() && g_PcCoopHostPaused;
}

void ShNet_CoopRequestPause(void)
{
    static unsigned int s_lastMs;
    unsigned int        now = ShNetPlat_Millis();
    unsigned char       b[2];
    if (!ShSession_Active() || ShSession_IsHost()) return;
    if (now - s_lastMs < 2000u) return;
    s_lastMs = now;
    b[0] = SHNET_CTRL_PAUSE_REQ;
    b[1] = 0;
    ShSession_SendGame(SHNET_MSG_S_CTRL, b, 2, ShSession_HostId(), 1);
    ShNet_PostLocalEvent("Asked the host to pause");
}

/* Every frame: the host tells the room when it pauses (twice on the edge,
 * then once a second while paused, so a late or lossy guest catches up) and
 * keeps the grant list current. */
static void ShNetG_CtrlTick(void)
{
    static int          s_wasPaused = -1;
    static unsigned int s_lastPauseMs;
    extern int          g_PcCoopGame;
    unsigned int        now = ShNetPlat_Millis();

    if (!g_PcCoopGame || !ShSession_Active())
    {
        g_PcCoopHostPaused = 0;
        s_wasPaused        = -1;
        return;
    }
    if (!ShSession_IsHost())
    {
        return;
    }
    {
        int paused = (g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_GamePaused) ? 1 : 0;
        if (paused != s_wasPaused || (paused && now - s_lastPauseMs >= 1000u))
        {
            unsigned char b[2];
            int           r;
            b[0] = SHNET_CTRL_PAUSE;
            b[1] = (unsigned char)paused;
            for (r = 0; r < (paused != s_wasPaused ? 2 : 1); r++)
            {
                ShSession_SendGame(SHNET_MSG_S_CTRL, b, 2, 0, 1);
            }
            s_wasPaused   = paused;
            s_lastPauseMs = now;
        }
    }
    if (s_permDirty || now - s_permSentMs >= 2000u)
    {
        unsigned char b[2 + SHSESSION_MAX_MEMBERS * 9];
        int           o = 0, i;
        b[o++] = SHNET_CTRL_PERM;
        b[o++] = (unsigned char)s_permN;
        for (i = 0; i < s_permN; i++)
        {
            ShnPutU64(b, &o, s_perm[i].id);
            b[o++] = s_perm[i].flags;
        }
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, o, 0, 1);
        s_permDirty  = 0;
        s_permSentMs = now;
    }
}

/* A control note from the room (drained with the other game messages). */
void ShNet_CoopCtrlIn(unsigned long long from, const unsigned char* p, int len)
{
    int fromHost = (from == ShSession_HostId());
    if (len < 1) return;
    switch (p[0])
    {
        case SHNET_CTRL_PAUSE:
            if (!fromHost || len < 2 || ShSession_IsHost()) return;
            if (g_PcCoopHostPaused != (p[1] != 0))
            {
                g_PcCoopHostPaused = (p[1] != 0);
                SH_DBG("[COOP] the host %s", g_PcCoopHostPaused ? "paused the game" : "resumed the game");
            }
            break;
        case SHNET_CTRL_PAUSE_REQ:
            if (!ShSession_IsHost()) return;
            {
                char line[96];
                snprintf(line, sizeof(line), "%s asks you to pause the game", ShNetG_MemberName(from));
                ShNet_PostLocalEvent(line);
            }
            break;
        case SHNET_CTRL_CUTSCENE:
            ShNetCut_CtrlIn(from, p, len);
            break;
        case SHNET_CTRL_VOICE:
            ShNetVoice_CtrlIn(from, p, len);
            break;
        case SHNET_CTRL_PERM:
            if (!fromHost || len < 2 || ShSession_IsHost()) return;
            {
                int n = p[1], i, o = 2, before = ShNet_CoopPermMine();
                s_permN = 0;
                for (i = 0; i < n && i < SHSESSION_MAX_MEMBERS && o + 9 <= len; i++)
                {
                    unsigned long long id = ShnGetU64(p, &o);
                    s_perm[s_permN].id    = id;
                    s_perm[s_permN].flags = p[o++];
                    s_permN++;
                }
                if (ShNet_CoopPermMine() != before)
                {
                    ShNet_PostLocalEvent((ShNet_CoopPermMine() & SHNET_PERM_CHEATS)
                                             ? "The host allowed you cheats and the console"
                                             : "The host took back cheats and the console");
                }
            }
            break;
        default:
            break;
    }
}

/* My health 0..100 for the overlay's own bar, -1 when not in plain gameplay. */
int ShNet_CoopSelfHealthPct(void)
{
    int hp;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
    {
        return -1;
    }
    hp = (int)(g_SysWork.playerWork.player.health >> 12);
    return hp < 0 ? 0 : hp > 100 ? 100 : hp;
}

/* Is this co-op game part of the living world (strangers' ghosts and
 * messages shown and ours placed)? Single-player: always. Co-op: the host's
 * choice, carried to guests in S_WORLD. Private is the default. */
int ShNet_CoopLivingWorld(void)
{
    extern int g_PcCoopGame;
    if (!g_PcCoopGame)
    {
        return 1;
    }
    if (!ShSession_Active() || ShSession_IsHost())
    {
        return g_PcConfig.onlineCoopLiving;
    }
    return (ShSession_WorldFlags() & SHSESSION_WF_LIVING) != 0;
}

/* Whether guests may open the console: the host's setting, read by guests
 * off the host's pose flags (the same way friendly fire travels). */
int ShNet_CoopGuestConsole(void)
{
    int i, n;
    if (!ShSession_Active() || ShSession_IsHost())
    {
        return g_PcConfig.onlineGuestConsole;
    }
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == ShSession_HostId() && m->poseMs != 0)
        {
            return (m->flags & SHNET_PF_GCONSOLE) != 0;
        }
    }
    return 0;
}

int ShNet_CoopFriendlyFire(void)
{
    int i, n;
    if (!ShSession_Active() || ShSession_IsHost())
    {
        return g_PcConfig.onlineFriendlyFire;
    }
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == ShSession_HostId() && m->poseMs != 0)
        {
            return (m->flags & SHNET_PF_FFIRE) != 0;
        }
    }
    return 0;
}

static void ShNetG_CoopGuestSpawn(int mapIdx)
{
    unsigned long long     hostId;
    const ShSessionMember* host = NULL;
    int                    i, n;

    if (!g_PcCoopGuestSpawn || !ShSession_Active() || ShSession_IsHost())
    {
        return;
    }

    hostId = ShSession_HostId();
    n      = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        if (m && m->steamId == hostId)
        {
            host = m;
            break;
        }
    }

    /* Need the host's live pose, and he must be on our map. Until then wait: the
     * player sits at the New Game spawn for the few frames it takes to hear him. */
    if (!host || host->poseMs == 0 || host->mapIdx != mapIdx)
    {
        return;
    }

    {
        s_SubCharacter*    hp = &g_SysWork.playerWork.player;
        s_CollisionSurface coll;
        Collision_SurfaceGet(&coll, host->x, host->z);
        hp->position.vx = host->x;
        hp->position.vz = host->z;
        if (coll.groundHeight != Q12(8.0f)) /* Q12(8.0f) is the "no floor here" sentinel */
        {
            hp->position.vy                    = coll.groundHeight;
            hp->properties.player.groundHeight = coll.groundHeight;
        }
        else
        {
            hp->position.vy = host->y;
        }
        hp->rotation.vy = (s16)host->rotY;
    }

    g_PcCoopGuestSpawn = 0;
    SH_DBG("[COOP] guest placed next to host on map %d", mapIdx);
}

void ShNet_OnMapChanged(int mapIdx)
{
    if (mapIdx == s_lastMap)
    {
        return;
    }
    s_lastMap      = mapIdx;
    s_deathLatched = 0;
    ShNet_RequestMemos();
    ShNet_RequestRoster();
    /* What friends see on the Steam friends list. The area name is the same
     * one the Discord presence line and the player list use. */
    ShSession_PublishPresence(Pc_MapAreaName(mapIdx), mapIdx);

    /* Co-op host: tell the session members to follow us into this map so a guest
     * boots (or re-boots) into the same area. Only the host dictates the world. */
    {
        extern int g_PcCoopGame;
        if (g_PcCoopGame && ShSession_IsHost())
        {
            ShSession_RequestWorld(mapIdx);
        }
    }
}

void ShNet_GameTick(void)
{
    int   inWorld;
    int   mapIdx;
    int   flags;
    short health;

    if (!s_initDone)
    {
        /* Deferred to the first frame rather than done in main_pc.c's init:
         * the config is fully parsed by then, and starting a thread before
         * SDL is up is the kind of ordering bug that only shows on someone
         * else's machine. */
        s_initDone = 1;
        ShNet_Init();
    }

    /* Co-op respawn state machine. Before the living-world gate: it belongs to
     * the co-op game, not to the server connection. */
    ShNetRespawn_Tick();
    ShNetCut_Tick();
    ShNetVoice_Tick();
    /* The in-game multiplayer menu, drawn with the game's own primitives over
     * the live world (MainLoop runs this after the state update and before
     * the ordering tables are walked). */
    if (Pc_CoopMenu_InGame() && g_GameWork.gameState == GameState_InGame)
    {
        Pc_CoopScreen_Draw();
    }

    /* Lobby chime: someone joined the room (member count went UP). Your own
     * arrival (0 -> 1) is silent. Before the enabled gate so co-op-only (no
     * living world) sessions chime too. */
    {
        static int s_lastMembers = 0;
        int mc = ShSession_Active() ? ShSession_MemberCount() : 0;
        if (mc > s_lastMembers && s_lastMembers > 0)
        {
            Sd_PlaySfx(Sfx_MenuConfirm, 0, Q8(0.30f));
        }
        s_lastMembers = mc;
    }

    /* Debug controls (console, cheat keys, binds) follow the config, except
     * for a guest in someone else's game: there the host decides, and the
     * default is off. Re-derived every frame so leaving a room restores the
     * player's own setting. */
    {
        extern int g_PcAllowDebugControls;
        int allow = g_PcConfig.allowDebugControls;
        if (ShSession_Active() && !ShSession_IsHost())
        {
            allow = ShNet_CoopGuestConsole() || (ShNet_CoopPermMine() & SHNET_PERM_CHEATS);
        }
        g_PcAllowDebugControls = allow;
    }
    ShNetG_CtrlTick();

    /* The host's word on the game, for S_WORLD. */
    ShSession_RequestWorldFlags(g_PcConfig.onlineCoopLiving ? SHSESSION_WF_LIVING : 0);

    /* A partner's skin needs its own skeleton proportions to draw right;
     * load them off the disc the moment a partner shows up wearing one. */
    if (ShSession_Active())
    {
        int i, n = ShSession_MemberCount();
        for (i = 0; i < n; i++)
        {
            const ShSessionMember* m = ShSession_Member(i);
            if (m && m->poseMs != 0 && m->steamId != ShSession_SelfId())
            {
                ShSessionPoseNow pn;
                Pc_PlayAs_RigPrefetch(m->charaId);
                /* And the weapon in their hand, so it can be drawn whatever
                 * we hold ourselves. */
                if (ShSession_MemberPoseNow(i, &pn) && pn.heldItem >= 0 && pn.heldItem < 0xFF)
                {
                    Pc_ItemPool_Want(pn.heldItem);
                }
            }
        }
    }

    if (!ShNet_Enabled())
    {
        return;
    }

    ShNetMemo_Tick();
    ShNetChat_Tick();

    /* A multiplayer game is never pausable -- pausing a world meant to be shared
     * is the one thing co-op must not do. Drive the coop seam from g_PcCoopGame
     * (set when the game was launched via the Multiplayer menu) so
     * ShNet_LiveWorld() / ShNet_PauseBlocked() report it; a single-player game
     * leaves it 0 and pauses normally. */
    {
        extern int g_PcCoopGame;
        if (g_PcCoopGame != ShNet_CoopActive())
        {
            ShNet_SetCoopActive(g_PcCoopGame, g_PcCoopGame ? "multiplayer game" : "single-player");
        }
    }

    inWorld = (g_GameWork.gameState == GameState_InGame) &&
              (g_SavegamePtr != NULL) &&
              !(g_SysWork.sysFlags & SysFlag_DemoActive);

    /* Co-op inventory: the world keeps running and we are still standing in
     * it, so keep sending where we are (flagged MENU) rather than vanishing
     * from our partners' screens. Items, hits and sounds wait for gameplay. */
    {
        extern int g_PcCoopGame;
        if (!inWorld && g_PcCoopGame && g_SavegamePtr != NULL &&
            g_GameWork.gameState == GameState_InventoryScreen && (int)g_SavegamePtr->mapIdx == s_lastMap)
        {
            ShNetG_PublishPose((int)g_SavegamePtr->mapIdx, ShNetG_BuildFlags() | SHNET_PF_MENU);
            ShNet_PumpToGameThread();
            return;
        }
    }

    if (!inWorld)
    {
        ShNet_PublishLocal(0, s_lastMap < 0 ? 0 : s_lastMap, ShNetG_LocalChara(), 0,
                           0, 0, 0, 0, 0, 0, 0);
        ShSession_PublishLocalPos(-1, 0, 0, NULL); /* no pose outside a map */
        ShSession_PublishPresence("In the menus", -1);
        ShNet_PumpToGameThread();
        return;
    }

    mapIdx = (int)g_SavegamePtr->mapIdx;
    if (mapIdx != s_lastMap)
    {
        ShNet_OnMapChanged(mapIdx);
    }

    flags = ShNetG_BuildFlags();

    {
        q19_12 hp = g_SysWork.playerWork.player.health;
        int    pct;
        if (hp < 0)
        {
            hp = 0;
        }
        pct = (int)(hp >> 12);
        if (pct > 999)
        {
            pct = 999;
        }
        health = (short)pct;
    }

    /* A private co-op game leaves no ghost in the living world. */
    ShNet_PublishLocal(ShNet_CoopLivingWorld() ? 1 : 0, mapIdx, ShNetG_LocalChara(), flags,
                       (int)g_SysWork.playerWork.player.position.vx,
                       (int)g_SysWork.playerWork.player.position.vy,
                       (int)g_SysWork.playerWork.player.position.vz,
                       (short)g_SysWork.playerWork.player.rotation.vy,
                       health,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.status,
                       (unsigned short)g_SysWork.playerWork.player.model.anim.keyframeIdx);

    /* Co-op presence: the same pose, to the Steam session members. */
    ShNetG_PublishPose(mapIdx, flags);

    /* Co-op shared items: hand out what we picked up, take in what others did. */
    ShNetG_SyncItems();
    /* Friendly fire: rebuild the hittable proxies for this frame, then apply
     * any hits that landed on us. */
    Pc_NetHit_Refresh();
    Pc_NetHit_ApplyIncoming();

    /* Co-op: place a joining guest next to the host once his pose is known. */
    ShNetG_CoopGuestSpawn(mapIdx);
    /* Co-op: the host's monsters out to everyone, puppets in from the host. */
    Pc_NetNpc_GameTick();
    {
        extern int g_PcCoopGame;
        if (g_PcCoopGame && ShSession_Active() && g_DeltaTime != Q12(0.0f))
        {
            ShNetG_PlayPartnerSfx(mapIdx);
        }
    }

    /* Death marker, once per death. Latched rather than edge-detected on the
     * health value alone: health sits at zero for the whole death animation
     * and the game-over screen, which would otherwise place a marker every
     * frame for several seconds. Cleared when the player is alive again,
     * which covers both continue and load. */
    if (g_PcConfig.onlineDeaths && ShNet_CoopLivingWorld())
    {
        const int dead = (g_SysWork.playerWork.player.health <= Q12(0.0f)) ||
                         (g_SysWork.sysState == SysState_GameOver);
        if (dead && !s_deathLatched)
        {
            s_deathLatched = 1;
            ShNet_ReportDeath((int)g_SysWork.playerWork.player.position.vx,
                              (int)g_SysWork.playerWork.player.position.vy,
                              (int)g_SysWork.playerWork.player.position.vz,
                              (short)g_SysWork.playerWork.player.rotation.vy);
            SH_DBG("[NET] death marker placed on map %d", mapIdx);
        }
        else if (!dead)
        {
            s_deathLatched = 0;
        }
    }

    ShNet_PumpToGameThread();

    /* Ghost names come from the roster, which is fetched once on connect and on
     * each map change. A player who reaches this map after that shows as "?"
     * (the placeholder set when a ghost is first seen), which also leaves the
     * nameplate blank. Ask for the roster again -- throttled -- whenever a ghost
     * is still unnamed, so late arrivals get named without polling the roster
     * every frame. */
    {
        int n = ShNet_GhostCount();
        int i;
        int unnamed = 0;
        for (i = 0; i < n; i++)
        {
            const ShNetGhost* g = ShNet_Ghost(i);
            if (g && (g->name[0] == '\0' || g->name[0] == '?'))
            {
                unnamed = 1;
                break;
            }
        }
        if (unnamed)
        {
            unsigned int now = ShNetPlat_Millis();
            if (now - s_lastNameReqMs >= SHNET_NAME_REQ_MS)
            {
                s_lastNameReqMs = now;
                ShNet_RequestRoster();
            }
        }
    }
}
