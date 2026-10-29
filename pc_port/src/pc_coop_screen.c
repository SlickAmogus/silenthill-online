/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_screen.c - the co-op menus drawn with the game's own primitives.
 *
 * The menu model lives in pc_coop_menu.c (pages, rows, selection, input);
 * this is its drawer, in the style of the native Achievements and Controls
 * screens (pc_native_menu_draw.c: FONT16 text, retail bullets and the gliding
 * gold underline, scroll arrows, black background on the title) with the
 * inventory's blue double frame around the panel. On the title screen it
 * replaces the menu art; in game it sits over the live world on a darkened
 * slab, since a co-op game never pauses.
 *
 * FONT16 is wide: about 27 glyphs fit the 320-wide text space, so a label
 * shares its row with its value by measuring, and whatever still does not
 * fit scrolls through its box (a word at a time, pausing at each end) rather
 * than being cut. On the 448-line title the small 8-px font exists and the
 * status and hint lines use it; in game only the tall font does.
 */

#include <stdio.h>
#include <string.h>

#include <SDL.h> /* SDL_HasScreenKeyboardSupport */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/item_screens.h" /* Gfx_Primitive2dTextureSet */
#include "lang_text.h"
#include "pc_coop_menu.h"
#include "pc_coop_screen.h"
#include "pc_discord.h" /* Pc_MapAreaName */
#include "pc_mouse_cursor.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"
#include "sh_net_session.h"

#define CS_X0        16
#define CS_X1        304
#define CS_FRAME_Y0  28
#define CS_FRAME_Y1  214
#define CS_HEADING_Y 8
#define CS_LIST_Y    38
#define CS_LIST_Y1   186          /* rows stop here */
#define CS_PITCH     18
#define CS_ENTRY_H   30           /* a two-line table entry */
#define CS_BULLET_X  28
#define CS_TEXT_X    (CS_BULLET_X + NM_BULLET_TEXT_DX)
#define CS_RIGHT_X   292
#define CS_STATUS_Y  190
#define CS_HINT_Y    216
#define CS_MAX_ROWS  64

#define CS_OT_LINE  6  /* with the rules */
#define CS_OT_SHADE 9  /* with the highlight's shade */

static int s_top;       /* first row drawn */
static int s_shownPrev; /* rows that fit last frame */
static int s_dragOn, s_dragLastY, s_dragAcc;

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
    setRGB0(poly, 112, 112, 112);
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

static int cs_w(const char* s)
{
    return Pc_LangMenuTextWidth(s);
}

/* The widest prefix of s (in whole glyphs) that fits maxW; its glyph count. */
static int cs_fit_count(const char* s, int maxW)
{
    char buf[NM_LINE_MAX];
    int  n = (int)strlen(s), k;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    for (k = n; k > 0; k--)
    {
        memcpy(buf, s, (size_t)k);
        buf[k] = '\0';
        if (cs_w(buf) <= maxW) return k;
    }
    return 0;
}

/* The field being typed into: its end, where the cursor is, always shows.
 * Scrolling it like any other long line moved the text under the typist. */
static void cs_text_tail(int x, int y, int maxW, int size, int color, const char* s)
{
    int off = 0, n = (int)strlen(s);
    while (off < n && cs_w(s + off) > maxW) off++;
    Nm_TextDrawVerbatim(x, y, size, color, s + off);
}

/* Text in a box: drawn whole when it fits, else a window of it that scrolls
 * through, pausing a moment at each end. Stable across frames because it is
 * keyed on the frame counter, not on who asked. */
static void cs_text_box(int x, int y, int maxW, int size, int color, const char* s)
{
    char buf[NM_LINE_MAX];
    int  n = (int)strlen(s), fit, over, period, phase, off, k;

    if (n == 0) return;
    if (cs_w(s) <= maxW)
    {
        Nm_TextDrawVerbatim(x, y, size, color, s);
        return;
    }
    fit = cs_fit_count(s, maxW);
    if (fit <= 0) return;
    over   = n - fit;                 /* glyphs that must scroll past */
    period = over + 6;                /* 3 beats parked at each end */
    phase  = (int)((g_TickCount / 24u) % (unsigned)period);
    off    = phase - 3;
    if (off < 0) off = 0;
    if (off > over) off = over;
    /* The window from off may hold more glyphs than `fit` where the glyphs
     * are narrower; measure again from there. */
    k = cs_fit_count(s + off, maxW);
    if (k > (int)sizeof(buf) - 1) k = (int)sizeof(buf) - 1;
    memcpy(buf, s + off, (size_t)k);
    buf[k] = '\0';
    Nm_TextDrawVerbatim(x, y, size, color, buf);
}

static void cs_text_right(int rx, int y, int color, const char* s)
{
    Nm_TextDrawVerbatim(rx - cs_w(s), y, NM_TEXT_TALL, color, s);
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

static void cs_rule(int y)
{
    Nm_RuleDraw(CS_X0 + 8, CS_X1 - 8, y, 0);
}

/* Rows of the current page in the window: table entries (two lines each, a
 * grey rule between) then the plain rows (Refresh / Back) as bullet rows. */
void Pc_CoopScreen_Draw(void)
{
    const int view    = Pc_CoopMenu_View();
    const int n       = Pc_CoopMenu_RowCount();
    const int sel     = Pc_CoopMenu_Selected();
    const int editing = Pc_CoopMenu_Editing();
    const int inGame  = Pc_CoopMenu_InGame();
    const int table   = (view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS);
    const int entries = (view == PC_COOP_VIEW_ROOMS) ? n - 2 : (view == PC_COOP_VIEW_PLAYERS) ? n - 1 : 0;
    const int small   = cs_interlaced() ? NM_TEXT_SMALL : NM_TEXT_TALL;
    int       rowY[CS_MAX_ROWS], rowH[CS_MAX_ROWS];
    int       listY   = CS_LIST_Y + (table ? 16 : 0);
    int       shown, y, i, hover = -1, dragged;
    char      text[NM_LINE_MAX], clean[NM_LINE_MAX];
    NmInput   in;

    if (!Pc_CoopMenu_IsOpen())
    {
        return;
    }

    NmInput_FromGame(&in);

    /* A held pointer dragged up or down scrolls the list a row per row of
     * travel: the phone gesture (the synthetic mouse a finger drives). The
     * press it began with is cancelled so the release picks nothing. */
    dragged = 0;
    if (in.mouseOk && !editing && Pc_MouseCursor_LeftHeld())
    {
        if (!s_dragOn)
        {
            s_dragOn    = 1;
            s_dragLastY = in.mouseY;
            s_dragAcc   = 0;
        }
        else
        {
            s_dragAcc  += in.mouseY - s_dragLastY;
            s_dragLastY = in.mouseY;
            while (s_dragAcc >= CS_PITCH)
            {
                s_dragAcc -= CS_PITCH;
                if (s_top > 0) s_top--;
                dragged = 1;
            }
            while (s_dragAcc <= -CS_PITCH)
            {
                s_dragAcc += CS_PITCH;
                if (s_top + s_shownPrev < n) s_top++;
                dragged = 1;
            }
            if (dragged) Pc_CoopMenu_CancelPress();
        }
    }
    else
    {
        s_dragOn = 0;
    }

    /* Lay out from s_top until the list area is full; keep the selection in
     * the window (rows have different heights, so this is iterative). A drag
     * moves the window instead, and the selection follows the finger below. */
    for (;;)
    {
        y = listY;
        shown = 0;
        for (i = s_top; i < n && i < s_top + CS_MAX_ROWS; i++)
        {
            int h = (i < entries) ? CS_ENTRY_H : CS_PITCH;
            if (y + h > CS_LIST_Y1) break;
            rowY[shown] = y;
            rowH[shown] = h;
            y += h;
            shown++;
        }
        if (dragged) break;
        if (sel < s_top && s_top > 0) { s_top = sel; continue; }
        if (sel >= s_top + shown && shown > 0 && s_top + shown < n) { s_top++; continue; }
        break;
    }
    if (s_top < 0) s_top = 0;
    s_shownPrev = shown;
    if (dragged && shown > 0 && (sel < s_top || sel >= s_top + shown))
    {
        Pc_CoopMenu_SetSelected(sel < s_top ? s_top : s_top + shown - 1);
    }

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
    if (in.mouseOk && !editing)
    {
        for (i = 0; i < shown; i++)
        {
            if (in.mouseX >= CS_X0 && in.mouseX < CS_X1 &&
                in.mouseY >= rowY[i] - 2 && in.mouseY < rowY[i] - 2 + rowH[i])
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
        const int hy = CS_LIST_Y - 2;
        if (view == PC_COOP_VIEW_ROOMS)
        {
            Nm_TextDrawVerbatim(CS_BULLET_X, hy, NM_TEXT_TALL, StringColorId_LightGrey, "GAME");
            cs_text_right(CS_RIGHT_X, hy, StringColorId_LightGrey, "PLAYERS");
        }
        else
        {
            Nm_TextDrawVerbatim(CS_BULLET_X, hy, NM_TEXT_TALL, StringColorId_LightGrey, "PLAYER");
            cs_text_right(CS_RIGHT_X, hy, StringColorId_LightGrey, "PING");
        }
        cs_rule(hy + 15);
        if (entries == 0)
        {
            Nm_TextDrawCentered(160, listY + 24, NM_TEXT_TALL, StringColorId_DarkGrey,
                                view == PC_COOP_VIEW_ROOMS ? "NO_OPEN_GAMES_YET" : "NOBODY_HERE");
        }
    }

    for (i = 0; i < shown; i++)
    {
        const int row = s_top + i;
        const int ry  = rowY[i];
        const int on  = (row == sel) && !editing;
        const int col = on ? StringColorId_Gold : StringColorId_White;

        Pc_CoopMenu_RowText(row, text, (int)sizeof(text));

        if (row < entries)
        {
            /* A list entry: name and the count on the first line, where it
             * is on the second, a rule under it. */
            char line1[64] = "", line2[64] = "", right[16] = "";
            if (view == PC_COOP_VIEW_ROOMS)
            {
                int  code = 0, players = 0, max = 0, map = -1;
                char host[40] = "";
                ShSession_RoomGet(row, &code, &players, &max, &map, host, (int)sizeof(host));
                snprintf(line1, sizeof(line1), "%s's Game", host[0] ? host : "Someone");
                if (ShSession_RoomLobby(row))
                    snprintf(line2, sizeof(line2), "Steam  -  %s", map >= 0 ? Pc_MapAreaName(map) : "in the lobby");
                else
                    snprintf(line2, sizeof(line2), "Room %d  -  %s", code, map >= 0 ? Pc_MapAreaName(map) : "in the lobby");
                snprintf(right, sizeof(right), "%d/%d", players, max);
            }
            else
            {
                const ShSessionMember* m = ShSession_Member(row);
                snprintf(line1, sizeof(line1), "%s", m ? m->name : "?");
                if (m && m->steamId == ShSession_HostId()) snprintf(line2, sizeof(line2), "Host");
                else if (m && m->mapIdx >= 0)              snprintf(line2, sizeof(line2), "%s", Pc_MapAreaName(m->mapIdx));
                else                                       snprintf(line2, sizeof(line2), "In the menus");
                if (m && m->pingMs >= 0) snprintf(right, sizeof(right), "%d ms", m->pingMs);
            }
            if (on)
            {
                Nm_HighlightDraw(CS_BULLET_X - 1, CS_RIGHT_X, ry + 14);
            }
            cs_clean(right, clean, sizeof(clean));
            cs_text_right(CS_RIGHT_X, ry, col, clean);
            {
                int rw = cs_w(clean);
                cs_clean(line1, clean, sizeof(clean));
                cs_text_box(CS_BULLET_X, ry, CS_RIGHT_X - rw - 8 - CS_BULLET_X, NM_TEXT_TALL, col, clean);
            }
            cs_clean(line2, clean, sizeof(clean));
            cs_text_box(CS_BULLET_X + 8, ry + 14, CS_RIGHT_X - CS_BULLET_X - 8, small,
                        on ? StringColorId_LightGrey : StringColorId_DarkGrey, clean);
            cs_rule(ry + CS_ENTRY_H - 3);
            continue;
        }

        if (on)
        {
            Nm_HighlightDraw(CS_BULLET_X - 1, CS_RIGHT_X, ry + 14);
        }
        Nm_BulletDraw(CS_BULLET_X, ry, on);
        {
            char label[NM_LINE_MAX], value[NM_LINE_MAX];
            if (cs_split(text, label, sizeof(label), value, sizeof(value)))
            {
                int vw;
                cs_clean(value, clean, sizeof(clean));
                vw = cs_w(clean);
                if (vw > 120) /* a value this long is the row's whole story */
                {
                    cs_text_box(CS_RIGHT_X - 120, ry, 120, NM_TEXT_TALL,
                                on ? StringColorId_Gold : StringColorId_LightGrey, clean);
                    vw = 120;
                }
                else
                {
                    cs_text_right(CS_RIGHT_X, ry, on ? StringColorId_Gold : StringColorId_LightGrey, clean);
                }
                cs_clean(label, clean, sizeof(clean));
                cs_text_box(CS_TEXT_X, ry, CS_RIGHT_X - vw - 10 - CS_TEXT_X, NM_TEXT_TALL, col, clean);
            }
            else
            {
                cs_clean(text, clean, sizeof(clean));
                cs_text_box(CS_TEXT_X, ry, CS_RIGHT_X - CS_TEXT_X, NM_TEXT_TALL, col, clean);
            }
        }
    }

    if ((s_top > 0 || s_top + shown < n) && !(editing && SDL_HasScreenKeyboardSupport()))
    {
        Nm_ScrollArrowDraw(160, CS_LIST_Y - 8, 1, s_top > 0);
        Nm_ScrollArrowDraw(160, CS_STATUS_Y - 4, 0, s_top + shown < n);
    }

    /* Status / the text being typed. */
    Pc_CoopMenu_StatusText(text, (int)sizeof(text));
    if (text[0])
    {
        cs_clean(text, clean, sizeof(clean));
        if (editing)
        {
            /* "Label:  typed_": the label stays put and only the typed text
             * is trimmed to keep its end, where the cursor is, in view. */
            char      label[NM_LINE_MAX], value[NM_LINE_MAX];
            /* A phone's keyboard covers the bottom half of a landscape
             * screen, status line and all: there the field goes up top,
             * under the heading, where the typing can be seen. */
            const int sx = CS_X0 + 8,
                      sy = SDL_HasScreenKeyboardSupport() ? CS_HEADING_Y + 16
                                                          : CS_STATUS_Y + (small == NM_TEXT_SMALL ? 4 : 0);
            int       lw = 0;
            if (!cs_split(text, label, sizeof(label), value, sizeof(value)))
            {
                label[0] = '\0';
                snprintf(value, sizeof(value), "%s", text);
            }
            if (label[0])
            {
                cs_clean(label, clean, sizeof(clean));
                Nm_TextDrawVerbatim(sx, sy, small, StringColorId_LightGrey, clean);
                lw = cs_w(clean) + 8;
            }
            cs_clean(value, clean, sizeof(clean));
            cs_text_tail(sx + lw, sy, CS_X1 - 8 - (sx + lw), small, StringColorId_Gold, clean);
        }
        else
            cs_text_box(CS_X0 + 8, CS_STATUS_Y + (small == NM_TEXT_SMALL ? 4 : 0), CS_X1 - CS_X0 - 16, small,
                        StringColorId_LightGrey, clean);
    }

    Pc_MouseCursor_Show();
}
