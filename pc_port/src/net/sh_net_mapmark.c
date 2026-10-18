/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_mapmark.c - co-op partners on the minimap and on the paper map.
 *
 * Both screens place Harry with func_80067914, which turns a world position
 * into a paper-map cell through each area's own layout (linear towns, per-
 * chunk school and hospital tables, rotated sewers, buildings pinned to one
 * spot). It only ever reads the local player, so a partner is placed by
 * standing them in Harry's position for the length of one query-only call
 * and putting Harry back: same thread, same frame, nothing else reads it in
 * between. The partner's floor travels in their pose (paperMap), because a
 * position alone cannot say which hospital floor someone is on.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"

#include "sh_net_mapmark.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"

#define MAPMARK_STALE_MS 3000

/* The ghosts' palette is dim so the fog swallows them; on a lit paper map
 * that reads as nothing. Same hash, so a partner's colour is stable, but
 * bright enough to find at a glance next to Harry's gold/blue arrow. */
static const u8 s_mapPalette[8][3] = {
    {  80, 200, 255 }, { 255, 110,  90 }, { 150, 255, 120 }, { 220, 130, 255 },
    { 255, 170,  60 }, {  90, 255, 220 }, { 255, 120, 190 }, { 200, 200, 255 }
};

void ShNet_MapMarkColor(unsigned long long id, u8* r, u8* g, u8* b)
{
    unsigned int h = (unsigned int)id * 2654435761u;
    const u8*    c = s_mapPalette[(h >> 28) & 7];
    *r = c[0];
    *g = c[1];
    *b = c[2];
}

int ShNet_MapMarks(ShNetMapMark* out, int max)
{
    extern int         g_PcCoopGame;
    const unsigned int now = ShNetPlat_Millis();
    int                i, n, got = 0;

    if (!g_PcCoopGame || !ShSession_Active() || g_SavegamePtr == NULL || out == NULL)
    {
        return 0;
    }
    n = ShSession_MemberCount();
    for (i = 0; i < n && got < max; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        ShSessionPoseNow       pn;
        if (!m || m->poseMs == 0 || m->steamId == ShSession_SelfId() ||
            m->mapIdx != (int)g_SavegamePtr->mapIdx || (int)(now - m->poseMs) > MAPMARK_STALE_MS)
        {
            continue;
        }
        if (!ShSession_MemberPoseNow(i, &pn))
        {
            continue;
        }
        out[got].x        = pn.x;
        out[got].z        = pn.z;
        out[got].rotY     = pn.rotY;
        out[got].paperMap = m->paperMapIdx;
        ShNet_MapMarkColor(m->steamId, &out[got].r, &out[got].g, &out[got].b);
        got++;
    }
    return got;
}

int Pc_PaperMapCellAt(int paperMapIdx, int x, int z, short rotY, s16* outMx, s16* outMz, q3_12* outAng)
{
    extern int      g_PcMapQueryOnly;
    extern q3_12    g_PcMapQueryAngle;
    s_SubCharacter* p = &g_SysWork.playerWork.player;
    const q19_12    sx = p->position.vx, sz = p->position.vz;
    const q3_12     sr = p->rotation.vy;
    const u8        sm = (u8)g_SavegamePtr->paperMapIdx;
    const int       sq = g_PcMapQueryOnly;
    const q3_12     sa = g_PcMapQueryAngle;
    s32             packed;

    p->position.vx = x;
    p->position.vz = z;
    p->rotation.vy = rotY;
    g_SavegamePtr->paperMapIdx = (u8)paperMapIdx;
    g_PcMapQueryAngle = rotY;
    g_PcMapQueryOnly  = 1;
    packed = func_80067914((s32)paperMapIdx, 0, 0, (u16)Q12(1.0f));
    if (outAng) *outAng = g_PcMapQueryAngle;
    g_PcMapQueryOnly  = sq;
    g_PcMapQueryAngle = sa;
    g_SavegamePtr->paperMapIdx = sm;
    p->position.vx = sx;
    p->position.vz = sz;
    p->rotation.vy = sr;

    if (packed == 0)
    {
        return 0;
    }
    if (outMx) *outMx = (s16)(packed & 0xFFFF);
    if (outMz) *outMz = (s16)((packed >> 16) & 0xFFFF);
    return 1;
}

/* Paper map screen: a partner arrow in the same place and shape as Harry's
 * (func_80067914's draw half), in the partner's colour, under Harry's. */
void Pc_PaperMapDrawPartners(int paperMapIdx, u16 arg1, u16 arg2, u16 arg3)
{
    static LINE_F4 s_line[2][SHSESSION_MAX_MEMBERS];
    static POLY_G3 s_poly[2][SHSESSION_MAX_MEMBERS];
    ShNetMapMark   mk[SHSESSION_MAX_MEMBERS];
    const int      buf = g_ActiveBufferIdx;
    int            i, n;

    /* L1/R1 hides Harry's arrow to show the map under it; hide ours too. */
    if (g_Controller0->heldBtnFlags & (ControllerFlag_L1 | ControllerFlag_R1))
    {
        return;
    }
    n = ShNet_MapMarks(mk, SHSESSION_MAX_MEMBERS);
    for (i = 0; i < n; i++)
    {
        s16   mx, mz;
        q3_12 ang;
        s32   sx, sy, a, sn, cs;
        s32   v[6];

        if (mk[i].paperMap >= 0 && mk[i].paperMap != paperMapIdx)
        {
            continue; /* another floor */
        }
        if (!Pc_PaperMapCellAt(paperMapIdx, mk[i].x, mk[i].z, mk[i].rotY, &mx, &mz, &ang))
        {
            continue;
        }
        sx = (((s32)mx - ((s32)arg1 - SCREEN_WIDTH / 2)) * SCREEN_WIDTH) / Q12_MULT_PRECISE(arg3, SCREEN_WIDTH) - SCREEN_WIDTH / 2;
        sy = (((s32)mz - ((s32)arg2 - SCREEN_HEIGHT / 2)) * SCREEN_HEIGHT) / Q12_MULT_PRECISE(arg3, SCREEN_HEIGHT) - SCREEN_HEIGHT / 2;
        a  = Math_AngleNormalizeSigned(ang);
        sn = Math_Sin(a);
        cs = Math_Cos(a);
        v[0] = sx + FP_FROM(sn * 6, Q12_SHIFT);
        v[1] = (sy + FP_FROM(cs * -6, Q12_SHIFT)) * 2;
        v[2] = sx + FP_FROM(cs * 4 - sn * 6, Q12_SHIFT);
        v[3] = (sy + FP_FROM(sn * 4 + cs * 6, Q12_SHIFT)) * 2;
        v[4] = sx + FP_FROM(cs * -4 - sn * 6, Q12_SHIFT);
        v[5] = (sy + FP_FROM(sn * -4 + cs * 6, Q12_SHIFT)) * 2;

        setLineF4(&s_line[buf][i]);
        setXY4(&s_line[buf][i], v[0], v[1], v[2], v[3], v[4], v[5], v[0], v[1]);
        setRGB0(&s_line[buf][i], 0x10, 0x10, 0x10);
        addPrim(&g_OrderingTable0[buf].org[2], &s_line[buf][i]);

        setPolyG3(&s_poly[buf][i]);
        setXY3(&s_poly[buf][i], v[0], v[1], v[2], v[3], v[4], v[5]);
        setRGB0(&s_poly[buf][i], mk[i].r, mk[i].g, mk[i].b);
        setRGB1(&s_poly[buf][i], mk[i].r / 2, mk[i].g / 2, mk[i].b / 2);
        setRGB2(&s_poly[buf][i], mk[i].r / 2, mk[i].g / 2, mk[i].b / 2);
        addPrim(&g_OrderingTable0[buf].org[2], &s_poly[buf][i]);
    }
}
