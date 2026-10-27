/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_mapmark.h - co-op partners on the minimap and the paper map screen.
 */
#ifndef SH_NET_MAPMARK_H
#define SH_NET_MAPMARK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int           x, z;      /* Q19.12 world, interpolated like the drawn body */
    short         rotY;
    int           paperMap;  /* their paper-map page (floor), -1 unknown */
    unsigned char r, g, b;
} ShNetMapMark;

/* Co-op partners on our map with a live pose. Returns the count written. */
int  ShNet_MapMarks(ShNetMapMark* out, int max);

/* Stable bright colour for a member on the map screens. */
void ShNet_MapMarkColor(unsigned long long id, unsigned char* r, unsigned char* g, unsigned char* b);

/* World position -> paper-map cell (-160..160, -120..120) and the map-space
 * heading, through the same per-area layout that places Harry. 0 = unmapped. */
int  Pc_PaperMapCellAt(int paperMapIdx, int x, int z, short rotY,
                       short* outMx, short* outMz, short* outAng);

/* Paper map screen: draws the partner arrows for the page on screen with the
 * screen's current scroll (arg1, arg2) and zoom (arg3). */
void Pc_PaperMapDrawPartners(int paperMapIdx, unsigned short arg1, unsigned short arg2, unsigned short arg3);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_MAPMARK_H */
