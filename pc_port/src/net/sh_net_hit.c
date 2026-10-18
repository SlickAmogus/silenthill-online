/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_hit.c - friendly fire. Design and rationale in sh_net_hit.h.
 *
 * The attack pipeline (bodyprog_combat_8008A058.c) resolves hits in world
 * space against s_SubCharacter collision cylinders, so a remote player becomes
 * hittable by giving it one: a proxy s_SubCharacter at its network position
 * wearing Harry's live hitbox. The game's own code does the geometry and the
 * damage roll; this file only owns the proxies, harvests what the roll wrote
 * into them, and applies incoming hits to the local player.
 */
#include <stdint.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/ray.h"
#include "pc_config.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_net_hit.h"
#include "sh_log.h"

/* A pose older than this is a player who stopped reporting; a frozen proxy
 * must not soak up hits. */
#define HIT_POSE_STALE_MS 3000

/* Chainsaw and drill re-apply damage every frame by design (func_8008B714
 * exempts them from the one-hit-per-swing latch). Against a player that is
 * one network hit per frame; cap melee to one hit per this interval. Ordinary
 * swings are already single hits through the latch and are unaffected. */
#define HIT_MELEE_MIN_MS 200

static s_SubCharacter     s_proxy[SHSESSION_MAX_MEMBERS];
static s_SubCharacter*    s_proxyPtr[SHSESSION_MAX_MEMBERS];
static unsigned long long s_proxyId[SHSESSION_MAX_MEMBERS];
static unsigned int       s_proxyMeleeMs[SHSESSION_MAX_MEMBERS];
static int                s_proxyCount;

static short s_pendYaw;
static int   s_pendYawSet;

int Pc_NetHit_ProxyCount(void)
{
    return s_proxyCount;
}

void* Pc_NetHit_Proxy(int i)
{
    return (i >= 0 && i < s_proxyCount) ? (void*)&s_proxy[i] : NULL;
}

int Pc_NetHit_ProxyIndex(const void* chara)
{
    const uintptr_t a = (uintptr_t)chara;
    const uintptr_t b = (uintptr_t)&s_proxy[0];
    const uintptr_t e = (uintptr_t)&s_proxy[SHSESSION_MAX_MEMBERS];
    if (a >= b && a < e)
    {
        return (int)((a - b) / sizeof(s_SubCharacter));
    }
    return -1;
}

/* The victim's handler (Player_ReceiveDamage) only knows ENEMY attack ids
 * (40..69): a player weapon id would deal the damage with no flinch and no
 * hurt sound. Map the weapon class onto enemy attacks that give the plain
 * directional torso flinch with a fitting hurt sound. 64/65 (Monster Cybil's
 * guns) are the only gunshot hurts; the handler subtracts 90 degrees from the
 * attacker heading for those two, so the sender adds it back. */
static int HitReaction(int weapon, int* yawAdjust)
{
    const int cls = D_800AD4C8[weapon].field_10; /* 3 gun, 1 blade, 2 blunt */
    *yawAdjust = 0;
    if (cls == 3)
    {
        *yawAdjust = Q12_ANGLE(90.0f);
        return weapon == WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap) ? 64 : 65;
    }
    return cls == 1 ? 57 : 43;
}

/* What this frame's attacks wrote into a proxy, off to its player. */
static void HarvestProxy(int i, unsigned int now)
{
    s_SubCharacter* px     = &s_proxy[i];
    const int       amount = px->damage.amount;
    const int       weapon = px->attackReceived;
    const VECTOR3   kb     = px->damage.position;
    int             reaction, yawAdj, yaw, scaled;

    px->damage.amount      = 0;
    px->damage.position.vx = 0;
    px->damage.position.vy = 0;
    px->damage.position.vz = 0;
    px->attackReceived     = NO_VALUE;

    if (amount <= 0 || weapon < 0 || weapon >= 70 || s_proxyId[i] == 0)
    {
        return;
    }

    reaction = HitReaction(weapon, &yawAdj);
    if (D_800AD4C8[weapon].field_10 != 3)
    {
        if (now - s_proxyMeleeMs[i] < HIT_MELEE_MIN_MS)
        {
            return;
        }
        s_proxyMeleeMs[i] = now;
    }

    /* Weapons are tuned for monsters with 200..40000 HP; Harry has 100. */
    scaled = (int)((float)amount * g_PcConfig.onlinePvpDamage);
    if (scaled < Q12(1.0f))
    {
        scaled = Q12(1.0f);
    }

    yaw = (g_SysWork.playerWork.player.rotation.vy + yawAdj) & 0xFFF;
    ShSession_QueueDamage(s_proxyId[i], scaled, kb.vx, kb.vy, kb.vz, reaction, (short)yaw);
    SH_DBG("[FF] hit player %llx: attack %d, %d.%02d HP", s_proxyId[i], weapon,
           scaled >> 12, ((scaled & 0xFFF) * 100) >> 12);
}

void Pc_NetHit_Refresh(void)
{
    const s_SubCharacter* me  = &g_SysWork.playerWork.player;
    const unsigned int    now = ShNetPlat_Millis();
    int                   i, n, myMap, count = 0;

    for (i = 0; i < s_proxyCount; i++)
    {
        HarvestProxy(i, now);
    }

    if (!g_PcConfig.onlineFriendlyFire || !ShSession_Active() || g_SavegamePtr == NULL)
    {
        s_proxyCount = 0;
        return;
    }

    myMap = (int)g_SavegamePtr->mapIdx;
    n     = ShSession_MemberCount();
    for (i = 0; i < n && count < SHSESSION_MAX_MEMBERS; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        s_SubCharacter*        px;
        ShSessionPoseNow       pn;

        if (m == NULL || m->poseMs == 0 || m->steamId == ShSession_SelfId() || m->mapIdx != myMap)
        {
            continue;
        }
        if (!(m->flags & SHNET_PF_ALIVE) || (m->flags & (SHNET_PF_MENU | SHNET_PF_CUTSCENE)))
        {
            continue;
        }
        if (!(m->flags & SHNET_PF_FFIRE)) /* they have not opted in: not a target */
        {
            continue;
        }
        if ((int)(now - m->poseMs) > HIT_POSE_STALE_MS)
        {
            continue;
        }

        px = &s_proxy[count];
        if (s_proxyId[count] != m->steamId)
        {
            memset(px, 0, sizeof(*px));
            s_proxyId[count]      = m->steamId;
            s_proxyMeleeMs[count] = 0;
            px->attackReceived    = NO_VALUE;
        }
        /* The interpolated pose the shooter is looking at, not the newest
         * sample: aiming at the drawn body must land on it. */
        if (!ShSession_MemberPoseNow(i, &pn))
        {
            pn.x = m->x; pn.y = m->y; pn.z = m->z; pn.rotY = m->rotY;
        }
        px->model.charaId = Chara_Harry;
        px->position.vx   = pn.x;
        px->position.vy   = pn.y;
        px->position.vz   = pn.z;
        px->rotation.vy   = pn.rotY;
        px->health        = Q12(100.0f);
        /* CharaFlag_Hit: skips the x4 "standing monster" melee multiplier that
         * the game never applies to a player target either. */
        px->flags = CharaFlag_Hit;
        /* Harry's live hitbox. Shape offsets are the player's own per-frame
         * rotation of its (zero) offsets; a proxy stands on its position. */
        px->collision                          = me->collision;
        px->collision.state                    = CharaCollisionState_Npc;
        px->collision.field_E4                 = NULL;
        px->collision.shapeOffsets.box.vx      = 0;
        px->collision.shapeOffsets.box.vz      = 0;
        px->collision.shapeOffsets.cylinder.vx = 0;
        px->collision.shapeOffsets.cylinder.vz = 0;

        s_proxyPtr[count] = px;
        count++;
    }
    for (i = count; i < SHSESSION_MAX_MEMBERS; i++)
    {
        s_proxyId[i] = 0;
    }
    s_proxyCount = count;
}

void* Pc_NetHit_BulletTrace(void* attacker, const void* from, void* offset, void* outTrace)
{
    extern s32  g_PcBulletHitActive;
    s_RayTrace* trace = (s_RayTrace*)outTrace;
    s_RayState* state;

    (void)attacker; /* never in the proxy list */
    trace->hasHit    = false;
    trace->character = NULL;
    if (s_proxyCount <= 0)
    {
        return NULL;
    }

    /* The same scratch the game's own bullet trace just finished with (see
     * Ray_CharaTraceQuery), with the same "fat hitbox" inflation so a player
     * is as hittable as an enemy. The trace also tests world geometry, so a
     * wall in between gives a hit with no character. */
    state               = (s_RayState*)PSX_SCRATCH;
    g_PcBulletHitActive = 1;
    if (Ray_TraceSetup(state, false, Q8(0.0f), (const VECTOR3*)from, (const VECTOR3*)offset,
                       Q12(0.0f), Q12(0.0f), s_proxyPtr, s_proxyCount))
    {
        trace->hasHit = Ray_TraceRun(trace, state);
    }
    g_PcBulletHitActive = 0;

    if (!trace->hasHit)
    {
        Ray_MissSet(trace, (const VECTOR3*)from, (const VECTOR3*)offset, state->rayDistance);
        return NULL;
    }
    return trace->character;
}

#define CLAMP_KB(v) ((v) > Q12(1.0f) ? Q12(1.0f) : (v) < -Q12(1.0f) ? -Q12(1.0f) : (v))

void Pc_NetHit_ApplyIncoming(void)
{
    s_SubCharacter* me = &g_SysWork.playerWork.player;
    int             amount, kbx, kby, kbz, reaction;
    short           yaw;

    while (ShSession_TakeDamage(&amount, &kbx, &kby, &kbz, &reaction, &yaw))
    {
        if (!g_PcConfig.onlineFriendlyFire)
        {
            static int s_said = 0;
            if (!s_said)
            {
                s_said = 1;
                SH_DBG("[FF] ignoring a hit from another player: friendly fire is off here");
            }
            continue;
        }
        if (g_SysWork.sysState != SysState_Gameplay || me->health <= Q12(0.0f))
        {
            continue;
        }
        /* Only the reactions HitReaction sends: the rest of 40..69 includes
         * grabs whose handlers expect a live monster and would hold the
         * player with nobody to let go. One hit can at most empty a full
         * health bar, and knockback stays within a monster blow's range. */
        if ((reaction != 43 && reaction != 57 && reaction != 64 && reaction != 65) ||
            amount <= 0 || amount > Q12(100.0f))
        {
            continue;
        }
        kbx = CLAMP_KB(kbx);
        kby = CLAMP_KB(kby);
        kbz = CLAMP_KB(kbz);

        /* Exactly what func_8008B714 writes for a monster's hit; the player
         * update consumes it next frame (Player_ReceiveDamage). There is no
         * NPC behind this hit, so the attacker heading travels separately. */
        me->damage.amount      += amount;
        me->damage.position.vx += kbx;
        me->damage.position.vy += kby;
        me->damage.position.vz += kbz;
        me->attackReceived      = (s8)reaction;
        me->field_40            = NO_VALUE;
        s_pendYaw    = yaw;
        s_pendYawSet = 1;
    }
}

int Pc_NetHit_TakePendingYaw(short* yaw)
{
    if (!s_pendYawSet)
    {
        return 0;
    }
    s_pendYawSet = 0;
    if (yaw)
    {
        *yaw = s_pendYaw;
    }
    return 1;
}
