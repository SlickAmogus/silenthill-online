/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_screen.c - the co-op menus drawn with the game's own primitives.
 *
 * The menu model lives in pc_coop_menu.c (pages, rows, selection, input);
 * this is its drawer, in the style of the native Achievements and Controls
 * screens (pc_native_menu_draw.c: FONT16 text, retail bullets and the gliding
 * gold underline, black background on the title) with the inventory's blue
 * double frame around the panel. On the title screen it replaces the menu
 * art; in game it sits over the live world on a darkened slab, since a
 * co-op game never pauses.
 *
 * Everything is in the 320x224 text space the native layer uses, so the one
 * layout serves the 448-line title and the 224-line game.
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/item_screens.h" /* Gfx_Primitive2dTextureSet */
#include "lang_text.h"
#include "pc_coop_menu.h"
#include "pc_coop_screen.h"
#include "pc_mouse_cursor.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"

#define CS_X0        20
#define CS_X1        300
#define CS_FRAME_Y0  28
#define CS_FRAME_Y1  214
#define CS_HEADING_Y 8
#define CS_LIST_Y    40
#define CS_PITCH     18
#define CS_VISIBLE   8
#define CS_BULLET_X  36
#define CS_TEXT_X    (CS_BULLET_X + NM_BULLET_TEXT_DX)
#define CS_RIGHT_X   284
#define CS_STATUS_Y  192
#define CS_HINT_Y    216
#define CS_TABLE_HDR_DY 14

#define CS_OT_LINE  6  /* with the rules */
#define CS_OT_SHADE 9  /* with the highlight's shade */

static int s_top; /* first row drawn */

static int cs_interlaced(void)
{
    return g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED;
}

static s16 cs_px(int x) { return (s16)(x - 160); }
static s16 cs_py(int y) { return (s16)(cs_interlaced() ? (y - 112) * 2 : (y - 112)); }

/* One of the inventory's blue lines (Gfx_Results_Save): (96,96,255) outside,
 * (32,32,255) two pixels in. */
static void cs_line(int x0, int y0, int x1, int y1, int inner)
{
    LINE_F2* line = (LINE_F2*)GsOUT_PACKET_P;
    u8       rg   = inner ? Q8_COLOR(0.125f) : Q8_COLOR(0.375f);

    setLineF2(line);
    setRGB0(line, rg, rg, 0xFF);
    setXY2(line, cs_px(x0), cs_py(y0), cs_px(x1), cs_py(y1));
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_LINE], line);
    GsOUT_PACKET_P = (PACKET*)(line + 1);
}

static void cs_frame(int x0, int y0, int x1, int y1)
{
    cs_line(x0, y0, x1, y0, 0);
    cs_line(x0, y1, x1, y1, 0);
    cs_line(x0, y0, x0, y1, 0);
    cs_line(x1, y0, x1, y1, 0);
    cs_line(x0 + 2, y0 + 2, x1 - 2, y0 + 2, 1);
    cs_line(x0 + 2, y1 - 2, x1 - 2, y1 - 2, 1);
    cs_line(x0 + 2, y0 + 2, x0 + 2, y1 - 2, 1);
    cs_line(x1 - 2, y0 + 2, x1 - 2, y1 - 2, 1);
}

/* In game: darken the panel's area so the text reads over the world. The
 * same subtractive blend the native highlight's shadow uses. */
static void cs_shade(int x0, int y0, int x1, int y1)
{
    POLY_F4* poly = (POLY_F4*)GsOUT_PACKET_P;

    setPolyF4(poly);
    setSemiTrans(poly, true);
    setRGB0(poly, 104, 104, 104);
    setXY4(poly, cs_px(x0), cs_py(y0), cs_px(x1), cs_py(y0), cs_px(x0), cs_py(y1), cs_px(x1), cs_py(y1));
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[CS_OT_SHADE], poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
    Gfx_Primitive2dTextureSet(0, 0, CS_OT_SHADE, 2);
}

/* FONT16 text: spaces become the font's blank glyph, everything else that the
 * font lacks is dropped, and the menus are uppercase like the retail ones. */
static void cs_clean(const char* in, char* out, size_t cap)
{
    Nm_SanitizeGlyphs(in, out, cap, 1);
}

static void cs_text(int x, int y, int color, const char* s)
{
    Nm_TextDrawVerbatim(x, y, NM_TEXT_TALL, color, s);
}

static void cs_text_fit(int x, int y, int maxW, int color, const char* s)
{
    char fit[NM_LINE_MAX];
    Nm_FitEllipsis(s, fit, sizeof(fit), maxW, Pc_LangMenuTextWidth);
    cs_text(x, y, color, fit);
}

static void cs_text_right(int rx, int y, int color, const char* s)
{
    cs_text(rx - Pc_LangMenuTextWidth(s), y, color, s);
}

/* "Label:  Value" rows show the value on the right. */
static int cs_split(const char* row, char* label, size_t lcap, char* value, size_t vcap)
{
    const char* c = strstr(row, ":  ");
    if (!c)
    {
        return 0;
    }
    {
        size_t n = (size_t)(c - row) + 1; /* keep the colon */
        if (n >= lcap) n = lcap - 1;
        memcpy(label, row, n);
        label[n] = '\0';
    }
    c += 3;
    while (*c == ' ') c++;
    snprintf(value, vcap, "%s", c);
    return 1;
}

static int cs_table_view(int view)
{
    return view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS;
}

/* Rows of the current page in the window: entries (table cells or bullet
 * rows) then the footer rows (Refresh / Back) as bullet rows. */
void Pc_CoopScreen_Draw(void)
{
    const int view    = Pc_CoopMenu_View();
    const int n       = Pc_CoopMenu_RowCount();
    const int sel     = Pc_CoopMenu_Selected();
    const int editing = Pc_CoopMenu_Editing();
    const int inGame  = Pc_CoopMenu_InGame();
    const int table   = cs_table_view(view);
    int       listY   = CS_LIST_Y + (table ? CS_TABLE_HDR_DY : 0);
    int       visible = table ? CS_VISIBLE - 1 : CS_VISIBLE;
    char      text[NM_LINE_MAX], clean[NM_LINE_MAX];
    int       i, hover = -1;
    NmInput   in;

    if (!Pc_CoopMenu_IsOpen())
    {
        return;
    }

    /* Keep the selected row in the window. */
    if (sel < s_top) s_top = sel;
    if (sel >= s_top + visible) s_top = sel - visible + 1;
    if (s_top > n - visible) s_top = n - visible;
    if (s_top < 0) s_top = 0;

    if (inGame)
    {
        cs_shade(CS_X0 - 4, CS_HEADING_Y - 4, CS_X1 + 4, CS_HINT_Y + 18);
    }
    else
    {
        Nm_BlackBackgroundDraw();
    }
    cs_frame(CS_X0, CS_FRAME_Y0, CS_X1, CS_FRAME_Y1);

    cs_clean(Pc_CoopMenu_Title(), clean, sizeof(clean));
    Nm_TextDrawCentered(160, CS_HEADING_Y, NM_TEXT_TALL, StringColorId_White, clean);

    /* The pointer: a row under it is the selected row (the model confirms on
     * click through Pc_MouseCursor_LeftHeld). */
    NmInput_FromGame(&in);
    if (in.mouseOk && !editing)
    {
        for (i = 0; i < visible && s_top + i < n; i++)
        {
            int y = listY + i * CS_PITCH - 2;
            if (in.mouseX >= CS_X0 && in.mouseX < CS_X1 && in.mouseY >= y && in.mouseY < y + CS_PITCH)
            {
                hover = s_top + i;
                break;
            }
        }
        if (in.wheel != 0 && n > 0)
        {
            Pc_CoopMenu_SetSelected(((sel - in.wheel) % n + n) % n);
        }
    }
    Pc_CoopMenu_SetHover(hover);

    /* Column headers. */
    if (table)
    {
        const int y = CS_LIST_Y - 2;
        if (view == PC_COOP_VIEW_ROOMS)
        {
            cs_text(CS_BULLET_X, y, StringColorId_LightGrey, "GAME");
            cs_text(164, y, StringColorId_LightGrey, "WHERE");
            cs_text_right(CS_RIGHT_X, y, StringColorId_LightGrey, "PLAYERS");
        }
        else
        {
            cs_text(CS_BULLET_X, y, StringColorId_LightGrey, "PLAYER");
            cs_text_right(CS_RIGHT_X, y, StringColorId_LightGrey, "PING");
        }
        Nm_RuleDraw(CS_X0 + 8, CS_X1 - 8, y + 15, 0);
    }

    for (i = 0; i < visible && s_top + i < n; i++)
    {
        const int row = s_top + i;
        const int y   = listY + i * CS_PITCH;
        const int on  = (row == sel) && !editing;
        const int col = on ? StringColorId_Gold : StringColorId_White;

        Pc_CoopMenu_RowText(row, text, (int)sizeof(text));
        if (on)
        {
            Nm_HighlightDraw(CS_BULLET_X - 1, CS_RIGHT_X, y + 14);
        }

        if (view == PC_COOP_VIEW_ROOMS && row < n - 2)
        {
            /* "<Host>'s Game  #code  (p/max)" -> three cells. */
            char game[64] = "", where[48] = "", cnt[16] = "";
            {
                extern int  ShSession_RoomGet(int i, int* code, int* players, int* max, int* mapIdx, char* host, int hostCap);
                extern unsigned long long ShSession_RoomLobby(int i);
                extern const char* Pc_MapAreaName(int idx);
                int  code = 0, players = 0, max = 0, map = -1;
                char host[40] = "";
                ShSession_RoomGet(row, &code, &players, &max, &map, host, (int)sizeof(host));
                snprintf(game, sizeof(game), "%s's Game", host[0] ? host : "Someone");
                if (ShSession_RoomLobby(row)) snprintf(where, sizeof(where), "Steam  %s", map >= 0 ? Pc_MapAreaName(map) : "lobby");
                else                          snprintf(where, sizeof(where), "%s", map >= 0 ? Pc_MapAreaName(map) : "In the lobby");
                snprintf(cnt, sizeof(cnt), "%d/%d", players, max);
            }
            cs_clean(game, clean, sizeof(clean));
            cs_text_fit(CS_BULLET_X, y, 120, col, clean);
            cs_clean(where, clean, sizeof(clean));
            cs_text_fit(164, y, 88, col, clean);
            cs_clean(cnt, clean, sizeof(clean));
            cs_text_right(CS_RIGHT_X, y, col, clean);
            continue;
        }
        if (view == PC_COOP_VIEW_PLAYERS && row < n - 1)
        {
            /* "Name  (NNms)" -> name and ping. */
            char* p = strstr(text, "  (");
            if (p)
            {
                char ping[16];
                *p = '\0';
                snprintf(ping, sizeof(ping), "%s", p + 3);
                if (ping[0] && ping[strlen(ping) - 1] == ')') ping[strlen(ping) - 1] = '\0';
                cs_clean(ping, clean, sizeof(clean));
                cs_text_right(CS_RIGHT_X, y, col, clean);
            }
            cs_clean(text, clean, sizeof(clean));
            cs_text_fit(CS_BULLET_X, y, 180, col, clean);
            continue;
        }

        Nm_BulletDraw(CS_BULLET_X, y, on);
        {
            char label[NM_LINE_MAX], value[NM_LINE_MAX];
            if (cs_split(text, label, sizeof(label), value, sizeof(value)))
            {
                cs_clean(label, clean, sizeof(clean));
                cs_text_fit(CS_TEXT_X, y, 120, col, clean);
                cs_clean(value, clean, sizeof(clean));
                cs_text_fit(CS_RIGHT_X - Pc_LangMenuTextWidth(clean) < CS_TEXT_X + 124
                                ? CS_TEXT_X + 124 : CS_RIGHT_X - Pc_LangMenuTextWidth(clean),
                            y, CS_RIGHT_X - (CS_TEXT_X + 124), on ? StringColorId_Gold : StringColorId_LightGrey, clean);
            }
            else
            {
                cs_clean(text, clean, sizeof(clean));
                cs_text_fit(CS_TEXT_X, y, CS_RIGHT_X - CS_TEXT_X, col, clean);
            }
        }
    }

    if (n > visible)
    {
        Nm_ScrollArrowDraw(160, CS_LIST_Y - 6, 1, s_top > 0);
        Nm_ScrollArrowDraw(160, CS_STATUS_Y - 6, 0, s_top + visible < n);
    }

    /* Status / the text being typed. */
    Pc_CoopMenu_StatusText(text, (int)sizeof(text));
    if (text[0])
    {
        cs_clean(text, clean, sizeof(clean));
        cs_text_fit(CS_X0 + 8, CS_STATUS_Y, CS_X1 - CS_X0 - 16, editing ? StringColorId_Gold : StringColorId_LightGrey, clean);
    }

    /* Key hints. */
    {
        const char* hint = editing ? "TYPE    ENTER CONFIRM    ESC CANCEL"
                         : Pc_CoopMenu_IsTopLevel() ? "ENTER CHOOSE    ESC CLOSE"
                                                    : "ENTER CHOOSE    ESC BACK";
        cs_clean(hint, clean, sizeof(clean));
        Nm_TextDrawCentered(160, CS_HINT_Y, NM_TEXT_TALL, StringColorId_DarkGrey, clean);
    }

    Pc_MouseCursor_Show();
}
