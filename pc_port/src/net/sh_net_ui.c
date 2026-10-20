/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_ui.c - every screen-space thing the online client draws.
 *
 * A fourth sibling of pc_quick_options.c / pc_rando_settings.c /
 * pc_confirm_dialog.c, and like them it repeats their hard-won GL constraints
 * rather than sharing code with them. Those constraints, each of which was
 * paid for:
 *
 *   - its OWN program / VAO / textures, with full state save and restore, and
 *     in particular the previously bound PROGRAM must be put back: PsyCross
 *     caches it and skips glUseProgram when it believes the right one is
 *     current, so leaving ours bound blacks out any later pass that never asks
 *     for a different shader.
 *   - LEGACY GLSL (attribute / varying / gl_FragColor, no #version), with
 *     `precision mediump float` under GL_ES so the ANGLE backends accept it.
 *   - ONE atlas texture, created at init and only ever sub-imaged. A texture
 *     created mid-frame inside the game's GL stream gets handed a name the
 *     game then recycles, and the panel starts sampling the game's art.
 *   - the vertex attribute pointers are re-asserted on every draw, not just at
 *     init: a stale a_uv is the "overlay blotches" class, and a 1x1 background
 *     texture hides it by making every uv sample the same texel.
 *   - all GL in Draw (post-capture). The game thread only reads input.
 *   - wall-clock timing, because the draw hook runs after the frame is
 *     captured and keeps running while the game is paused.
 *
 * What it draws:
 *   - the player list (key_online_players, default F11)
 *   - the memo composer (key_online_memo, default M)
 *   - the marker the player is standing on, at the bottom of the screen
 *   - the server's join / leave / death feed, top right
 *   - a connection line while the client is not yet connected
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"

#include "sh_net.h"
#include "sh_net_memo.h"
#include "sh_net_chat.h"
#include "sh_net_session.h"
#include "sh_net_cutscene.h"
#include "sh_net_voice.h"
#include "sh_net_respawn.h" /* "YOU DIED / respawning in N" */
#include "pc_coop_menu.h"
#include "pc_mouse_cursor.h"
#include "pc_config.h"
#include "pc_discord.h" /* Pc_MapAreaName */
#include "sh_log.h"

#define NU_ATLAS_W   1024
#define NU_ATLAS_H   1024
#define NU_ATLAS_PAD 2
#define NU_SLOT_MAX  192
#define NU_CACHE_MAX 128
#define NU_TEXT_MAX  72

/* Rows the player list shows at once before it starts saying "and N more". */
#define NU_LIST_ROWS 14
/* Rows each composer column shows around the selection. */
#define NU_COMPOSER_ROWS 9

#define NU_EVENT_SHOW_MS 6000
#define NU_EVENT_FADE_MS 1200
#define NU_EVENT_SLOTS   5

/* How long the master server has to stay unreachable before the player is
 * told. Long enough that a reconnect nobody noticed stays unnoticed. */
#define NU_LOST_QUIET_MS 15000

int g_ShNetPlayerListOpen;

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo, s_atlas;
static GLint  s_locColor;
static int    s_glReady;   /* 0 = untried, -1 = failed, 1 = ready */

static struct { int x, y, w, h, cw, ch, free; } s_slot[NU_SLOT_MAX];
static int s_slotCount;
/* Set when an upload is refused because the atlas is full. The next Draw resets
 * the atlas and rebuilds from scratch -- one frame's cost, always recovers --
 * instead of silently dropping text forever (which made option values and rows
 * vanish once the varied co-op/memo strings had filled all 192 slots). */
static int s_atlasExhausted;
static int s_atlasX, s_atlasY, s_atlasRowH;

/* The mouse cursor is a fixed bitmap drawn every frame. It is uploaded ONCE into
 * a permanent atlas slot and the handle reused, because the per-frame path does
 * not free its slot the way the text cache does -- re-uploading each frame
 * leaked a slot per frame and ran the 192-slot atlas dry in a few seconds, at
 * which point the cursor AND every text row stopped drawing. Reset to 0 whenever
 * the atlas is rebuilt so it re-uploads. */
static GLuint s_curSlot = 0;

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk;
static int            s_fontTried;

static struct
{
    char   text[NU_TEXT_MAX];
    int    px;
    GLuint slot;
    int    w, h;
    unsigned lastUse;
} s_cache[NU_CACHE_MAX];
static unsigned s_useClock;

static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static unsigned int s_lostSinceMs;

static GLuint Nu_Shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[NETUI] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void Nu_AtlasReset(void)
{
    s_atlasX    = NU_ATLAS_PAD + 1 + NU_ATLAS_PAD;
    s_atlasY    = NU_ATLAS_PAD;
    s_atlasRowH = 1;
    /* Slot 0 is the permanent white texel every panel background and divider
     * draws from; handing it to a string would repaint the panel with a word. */
    s_slot[0].x  = NU_ATLAS_PAD;
    s_slot[0].y  = NU_ATLAS_PAD;
    s_slot[0].w  = 1;
    s_slot[0].h  = 1;
    s_slot[0].cw = 1;
    s_slot[0].ch = 1;
    s_slot[0].free = 0;
    s_slotCount = 1;
    s_curSlot   = 0; /* the cursor must re-upload into the rebuilt atlas */
}

static void Nu_GlInit(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";

    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0, prevProg = 0, prevTex = 0;

    s_glReady = -1;
    vs = Nu_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Nu_Shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
    {
        return;
    }

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[NETUI] link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");

    glGenTextures(1, &s_atlas);
    if (!s_atlas)
    {
        glUseProgram((GLuint)prevProg);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* A MIN filter that wants mip levels which do not exist makes the texture
     * INCOMPLETE, and an incomplete texture samples as opaque black whatever
     * else is correct. Pinning the chain at level 0 puts that out of reach. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NU_ATLAS_W, NU_ATLAS_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    {
        const unsigned char one[4] = { 255, 255, 255, 255 };
        glTexSubImage2D(GL_TEXTURE_2D, 0, NU_ATLAS_PAD, NU_ATLAS_PAD, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, one);
    }
    Nu_AtlasReset();
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glUseProgram((GLuint)prevProg);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    s_glReady = 1;
    SH_DBG("[NETUI] GL ready (atlas %dx%d id=%u)", NU_ATLAS_W, NU_ATLAS_H, (unsigned)s_atlas);
}

/* Returns a slot handle (index + 1), or 0. */
static GLuint Nu_Upload(const unsigned char* rgba, int w, int h)
{
    int idx;

    if (s_glReady != 1 || !s_atlas || w <= 0 || h <= 0)
    {
        return 0;
    }
    if (w > NU_ATLAS_W - 2 * NU_ATLAS_PAD || h > NU_ATLAS_H - 2 * NU_ATLAS_PAD)
    {
        return 0;
    }

    /* Recycle first: a value that re-bakes every frame (a ping in
     * milliseconds) would otherwise burn one slot per frame and the atlas
     * would run dry within a minute of the panel being open. Best fit, so a
     * short string does not claim a long string's rectangle. */
    {
        int best = -1, bestArea = 0;
        for (idx = 1; idx < s_slotCount; idx++)
        {
            int area;
            if (!s_slot[idx].free || s_slot[idx].cw < w || s_slot[idx].ch < h)
            {
                continue;
            }
            area = s_slot[idx].cw * s_slot[idx].ch;
            if (best < 0 || area < bestArea) { best = idx; bestArea = area; }
        }
        if (best >= 0)
        {
            s_slot[best].w    = w;
            s_slot[best].h    = h;
            s_slot[best].free = 0;
            glBindTexture(GL_TEXTURE_2D, s_atlas);
            glTexSubImage2D(GL_TEXTURE_2D, 0, s_slot[best].x, s_slot[best].y, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            return (GLuint)(best + 1);
        }
    }

    if (s_slotCount >= NU_SLOT_MAX)
    {
        s_atlasExhausted = 1;
        return 0;
    }
    if (s_atlasX + w + NU_ATLAS_PAD > NU_ATLAS_W)
    {
        s_atlasX     = NU_ATLAS_PAD;
        s_atlasY    += s_atlasRowH + NU_ATLAS_PAD;
        s_atlasRowH  = 0;
    }
    if (s_atlasY + h + NU_ATLAS_PAD > NU_ATLAS_H)
    {
        s_atlasExhausted = 1;
        return 0;
    }

    idx = s_slotCount++;
    s_slot[idx].x  = s_atlasX;
    s_slot[idx].y  = s_atlasY;
    s_slot[idx].w  = w;
    s_slot[idx].h  = h;
    s_slot[idx].cw = w;
    s_slot[idx].ch = h;
    s_slot[idx].free = 0;

    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glTexSubImage2D(GL_TEXTURE_2D, 0, s_atlasX, s_atlasY, w, h,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    s_atlasX += w + NU_ATLAS_PAD;
    if (h > s_atlasRowH)
    {
        s_atlasRowH = h;
    }
    return (GLuint)(idx + 1);
}

/* Quad in NDC (yTop > yBot). `slot` is an atlas handle, not a texture name. */
static void Nu_Quad(GLuint slot, float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    float v[6][4];
    float u0, v0, u1, v1;
    int   idx = (int)slot - 1;

    if (slot == 0 || idx < 0 || idx >= s_slotCount || !s_atlas)
    {
        return;
    }

    /* Half-texel inset: with LINEAR filtering an edge tap would otherwise
     * reach into a neighbouring slot's padding. */
    u0 = ((float)s_slot[idx].x + 0.5f) / (float)NU_ATLAS_W;
    v0 = ((float)s_slot[idx].y + 0.5f) / (float)NU_ATLAS_H;
    u1 = ((float)(s_slot[idx].x + s_slot[idx].w) - 0.5f) / (float)NU_ATLAS_W;
    v1 = ((float)(s_slot[idx].y + s_slot[idx].h) - 0.5f) / (float)NU_ATLAS_H;

    v[0][0] = x0; v[0][1] = yTop; v[0][2] = u0; v[0][3] = v0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = u0; v[1][3] = v1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = u1; v[2][3] = v0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = u1; v[3][3] = v0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = u0; v[4][3] = v1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = u1; v[5][3] = v1;

    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, s_atlas);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* Solid fill, straight from the white texel. */
static void Nu_Fill(float x0, float yTop, float x1, float yBot,
                    float r, float g, float b, float a)
{
    Nu_Quad(1, x0, yTop, x1, yBot, r, g, b, a);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static unsigned char* Nu_ReadFile(const char* path, long* outSize)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (outSize) *outSize = n;
    return buf;
}

static void Nu_FontInit(void)
{
    static const char* const paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/Library/Fonts/Arial.ttf"
    };
    int i;

    if (s_fontTried)
    {
        return;
    }
    s_fontTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        long           sz   = 0;
        unsigned char* data = Nu_ReadFile(paths[i], &sz);
        if (!data)
        {
            continue;
        }
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            /* stb_truetype does not copy the file: it reads this buffer on
             * every glyph, so it must outlive the process. Never freed. */
            s_fontData = data;
            s_fontOk   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[NETUI] no usable font - the online panels will draw without text");
}

/* Rasterize one line of ASCII to white-with-coverage-alpha, trimmed to the ink
 * so centring and right-alignment use the real width. */
unsigned char* ShNetUi_RasterizeText(const char* text, int px, int* outW, int* outH)
{
    const char*    p;
    float          scale, penX, baseY;
    int            asc, desc, gap, W, H, i, stride, x, y;
    const int      pad = 2;
    int            prev = 0;
    unsigned char *cov, *rgba, *scratch;

    Nu_FontInit();
    if (!s_fontOk || !text || !text[0] || px < 6)
    {
        return NULL;
    }
    scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)((float)px * 0.66f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf((float)px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 1024 || H > 128)
    {
        return NULL;
    }
    stride = W;
    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return NULL; }

    penX  = (float)pad;
    baseY = (float)pad + (float)asc * scale;
    p     = text;
    while (*p)
    {
        int   cp = (unsigned char)*p++;
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev) penX += (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f,
                                            &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0; gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx; unsigned char c;
                    if (dx < 0 || dx >= W) continue;
                    c = scratch[sy * gw + sx];
                    if (c > cov[dy * W + dx]) cov[dy * W + dx] = c;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += (float)adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }

    { int inkW = (int)ceilf(penX) + pad; if (inkW > 0 && inkW < W) W = inkW; }

    /* White glyph with a dark halo: alpha is coverage dilated by one pixel,
     * colour is the glyph where it is solid and near-black in the halo ring. */
    for (y = 0; y < H; y++)
    {
        for (x = 0; x < W; x++)
        {
            int   c = cov[y * stride + x];
            int   dil = c, dx, dy;
            int   o = (y * W + x) * 4;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                    {
                        int nc = cov[ny * stride + nx];
                        if (nc > dil) dil = nc;
                    }
                }
            rgba[o + 0] = (unsigned char)c;
            rgba[o + 1] = (unsigned char)c;
            rgba[o + 2] = (unsigned char)c;
            rgba[o + 3] = (unsigned char)dil;
        }
    }

    free(cov); free(scratch);
    if (outW) *outW = W;
    if (outH) *outH = H;
    return rgba;
}

static GLuint Nu_Bake(const char* text, int px, int* outW, int* outH)
{
    const char*    p;
    float          scale, penX, baseY;
    int            asc, desc, gap, W, H, i, stride;
    const int      pad = 1;
    int            prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint         slot;

    if (!s_fontOk || !text || !text[0] || px < 6)
    {
        return 0;
    }

    scale = stbtt_ScaleForPixelHeight(&s_font, (float)px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);

    W = (int)((float)px * 0.66f * (float)strlen(text)) + 8 * pad;
    H = (int)ceilf((float)px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 2048 || H > 256)
    {
        return 0;
    }
    stride = W;

    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch)
    {
        free(cov); free(rgba); free(scratch);
        return 0;
    }

    penX  = (float)pad;
    baseY = (float)pad + (float)asc * scale;
    p     = text;
    while (*p)
    {
        int   cp = (unsigned char)*p++;
        int   gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;

        if (prev)
        {
            penX += (float)stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        }
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f,
                                            &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw,
                                              scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int           dx = (int)penX + gx0 + sx;
                    unsigned char c;
                    if (dx < 0 || dx >= W) continue;
                    c = scratch[sy * gw + sx];
                    if (c > cov[dy * W + dx]) cov[dy * W + dx] = c;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += (float)adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }

    {
        int inkW = (int)ceilf(penX) + pad;
        if (inkW > 0 && inkW < W) W = inkW;
    }
    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[(i / W) * stride + (i % W)];
    }

    slot = Nu_Upload(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return slot;
}

/* Baked strings, keyed on (text, size). A ping value re-bakes constantly and a
 * label never does; one cache with LRU eviction handles both without the
 * caller having to know which it is holding. */
static GLuint Nu_Text(const char* text, int px, int* outW, int* outH)
{
    int i, victim = -1;
    unsigned oldest = 0xFFFFFFFFu;

    if (!text || !text[0])
    {
        return 0;
    }
    s_useClock++;

    for (i = 0; i < NU_CACHE_MAX; i++)
    {
        if (s_cache[i].slot && s_cache[i].px == px &&
            strncmp(s_cache[i].text, text, NU_TEXT_MAX - 1) == 0)
        {
            s_cache[i].lastUse = s_useClock;
            if (outW) *outW = s_cache[i].w;
            if (outH) *outH = s_cache[i].h;
            return s_cache[i].slot;
        }
        if (!s_cache[i].slot)
        {
            victim = i;
            oldest = 0;
        }
        else if (s_cache[i].lastUse < oldest)
        {
            victim = i;
            oldest = s_cache[i].lastUse;
        }
    }

    if (victim < 0)
    {
        return 0;
    }
    if (s_cache[victim].slot)
    {
        int idx = (int)s_cache[victim].slot - 1;
        if (idx > 0 && idx < s_slotCount)
        {
            s_slot[idx].free = 1;
        }
    }
    memset(&s_cache[victim], 0, sizeof(s_cache[victim]));

    {
        int w = 0, h = 0;
        GLuint slot = Nu_Bake(text, px, &w, &h);
        if (!slot)
        {
            return 0;
        }
        SDL_strlcpy(s_cache[victim].text, text, NU_TEXT_MAX);
        s_cache[victim].px      = px;
        s_cache[victim].slot    = slot;
        s_cache[victim].w       = w;
        s_cache[victim].h       = h;
        s_cache[victim].lastUse = s_useClock;
        if (outW) *outW = w;
        if (outH) *outH = h;
        return slot;
    }
}

/* Draws `text` with its LEFT edge at viewport pixel x and its TOP at y (y
 * measured down from the top). Returns the drawn width in pixels. */
static float Nu_DrawText(const char* text, float x, float y, int px,
                         float r, float g, float b, float a)
{
    int    w = 0, h = 0;
    GLuint slot = Nu_Text(text, px, &w, &h);
    float  x0, x1, yT, yB;

    if (!slot)
    {
        return 0.0f;
    }
    x0 = (x / s_vpW) * 2.0f - 1.0f;
    x1 = ((x + (float)w) / s_vpW) * 2.0f - 1.0f;
    yT = 1.0f - (y / s_vpH) * 2.0f;
    yB = 1.0f - ((y + (float)h) / s_vpH) * 2.0f;
    Nu_Quad(slot, x0, yT, x1, yB, r, g, b, a);
    return (float)w;
}

static float Nu_TextWidth(const char* text, int px)
{
    int w = 0, h = 0;
    return Nu_Text(text, px, &w, &h) ? (float)w : 0.0f;
}

static void Nu_DrawTextRight(const char* text, float xRight, float y, int px,
                             float r, float g, float b, float a)
{
    Nu_DrawText(text, xRight - Nu_TextWidth(text, px), y, px, r, g, b, a);
}

static void Nu_DrawTextCentered(const char* text, float cx, float y, int px,
                                float r, float g, float b, float a)
{
    Nu_DrawText(text, cx - Nu_TextWidth(text, px) * 0.5f, y, px, r, g, b, a);
}

/* Panel in viewport pixels: a translucent slab with a one-pixel edge. */
static void Nu_Panel(float x, float y, float w, float h, float alpha)
{
    const float x0 = (x / s_vpW) * 2.0f - 1.0f;
    const float x1 = ((x + w) / s_vpW) * 2.0f - 1.0f;
    const float yT = 1.0f - (y / s_vpH) * 2.0f;
    const float yB = 1.0f - ((y + h) / s_vpH) * 2.0f;
    const float px = 2.0f / s_vpW;
    const float py = 2.0f / s_vpH;

    Nu_Fill(x0, yT, x1, yB, 0.04f, 0.045f, 0.05f, 0.86f * alpha);
    Nu_Fill(x0, yT, x1, yT - py, 0.45f, 0.44f, 0.40f, 0.55f * alpha);
    Nu_Fill(x0, yB + py, x1, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
    Nu_Fill(x0, yT, x0 + px, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
    Nu_Fill(x1 - px, yT, x1, yB, 0.45f, 0.44f, 0.40f, 0.35f * alpha);
}

/* ------------------------------------------------------------------ */
/* Event feed                                                          */
/* ------------------------------------------------------------------ */

static struct { char text[SHNET_EVENT_MAX]; unsigned int ms; } s_events[NU_EVENT_SLOTS];
static int s_eventCount;

static void Nu_PumpEvents(void)
{
    char line[SHNET_EVENT_MAX];
    const unsigned int now = SDL_GetTicks();
    int   i;

    while (ShNet_PopEvent(line, (int)sizeof(line)))
    {
        if (s_eventCount == NU_EVENT_SLOTS)
        {
            memmove(&s_events[0], &s_events[1], sizeof(s_events[0]) * (NU_EVENT_SLOTS - 1));
            s_eventCount--;
        }
        SDL_strlcpy(s_events[s_eventCount].text, line, SHNET_EVENT_MAX);
        s_events[s_eventCount].ms = now;
        s_eventCount++;
    }

    i = 0;
    while (i < s_eventCount)
    {
        if (now - s_events[i].ms > NU_EVENT_SHOW_MS + NU_EVENT_FADE_MS)
        {
            memmove(&s_events[i], &s_events[i + 1],
                    sizeof(s_events[0]) * (size_t)(s_eventCount - i - 1));
            s_eventCount--;
        }
        else
        {
            i++;
        }
    }
}

static void Nu_DrawEvents(int px)
{
    const unsigned int now = SDL_GetTicks();
    float y = s_vpH * 0.06f;
    int   i;

    if (!g_PcConfig.onlineEvents)
    {
        return;
    }
    for (i = 0; i < s_eventCount; i++)
    {
        unsigned int age = now - s_events[i].ms;
        float        a   = 1.0f;
        if (age > NU_EVENT_SHOW_MS)
        {
            a = 1.0f - (float)(age - NU_EVENT_SHOW_MS) / (float)NU_EVENT_FADE_MS;
        }
        if (a <= 0.0f)
        {
            continue;
        }
        Nu_DrawTextRight(s_events[i].text, s_vpW - s_vpH * 0.03f, y, px,
                         0.78f, 0.76f, 0.70f, a);
        y += (float)px * 1.3f;
    }
}

/* ------------------------------------------------------------------ */
/* Player list                                                         */
/* ------------------------------------------------------------------ */

static void Nu_DrawPlayerList(int px)
{
    const int   big   = (int)((float)px * 1.25f);
    const float pad   = (float)px * 0.9f;
    const float rowH  = (float)px * 1.45f;
    const int   count = ShNet_PeerCount();
    const int   shown = (count > NU_LIST_ROWS) ? NU_LIST_ROWS : count;
    const float w     = s_vpW * 0.42f < 420.0f ? 420.0f : s_vpW * 0.42f;
    const int   sess  = ShSession_MemberCount();
    const float h     = pad * 2.0f + (float)big * 1.5f + (float)px * 1.4f +
                        rowH * (float)(shown + 2) +
                        (sess > 0 ? rowH * (float)(sess + 2) : 0.0f);
    const float x     = (s_vpW - w) * 0.5f;
    const float y     = s_vpH * 0.12f;
    float       ty;
    int         i;
    char        buf[128];

    Nu_Panel(x, y, w, h, 1.0f);

    ty = y + pad;
    Nu_DrawText(ShNet_ServerName()[0] ? ShNet_ServerName() : "Silent Hill Online",
                x + pad, ty, big, 0.86f, 0.83f, 0.74f, 1.0f);
    {
        const int st = ShNet_Status();
        if (st == SHNET_ST_CONNECTED)
        {
            snprintf(buf, sizeof(buf), "%d online   %d ms", ShNet_PeerTotal(), ShNet_PingMs());
            Nu_DrawTextRight(buf, x + w - pad, ty, big, 0.55f, 0.62f, 0.55f, 1.0f);
        }
        else
        {
            Nu_DrawTextRight(ShNet_StatusText(), x + w - pad, ty, big,
                             0.72f, 0.52f, 0.42f, 1.0f);
        }
    }
    ty += (float)big * 1.5f;

    if (ShNet_Motd()[0])
    {
        Nu_DrawText(ShNet_Motd(), x + pad, ty, px, 0.58f, 0.56f, 0.52f, 1.0f);
    }
    ty += (float)px * 1.4f;

    {
        const float x0 = (x + pad) / s_vpW * 2.0f - 1.0f;
        const float x1 = (x + w - pad) / s_vpW * 2.0f - 1.0f;
        const float yy = 1.0f - (ty / s_vpH) * 2.0f;
        Nu_Fill(x0, yy, x1, yy - 2.0f / s_vpH, 0.4f, 0.39f, 0.36f, 0.5f);
    }
    ty += rowH * 0.4f;

    if (count == 0)
    {
        Nu_DrawText(ShNet_Status() == SHNET_ST_CONNECTED
                        ? "No one else is here."
                        : "Not connected.",
                    x + pad, ty, px, 0.5f, 0.48f, 0.45f, 1.0f);
    }

    for (i = 0; i < shown; i++)
    {
        const ShNetPeer* p = ShNet_Peer(i);
        const int        me = p && p->playerId == ShNet_SelfId();
        float            r = me ? 0.85f : 0.72f;
        float            g = me ? 0.80f : 0.70f;
        float            b = me ? 0.58f : 0.66f;

        if (!p)
        {
            continue;
        }
        Nu_DrawText(p->name, x + pad, ty, px, r, g, b, 1.0f);
        Nu_DrawText(p->mapIdx >= 0 && p->mapIdx < 64 ? Pc_MapAreaName(p->mapIdx) : "-",
                    x + w * 0.38f, ty, px, 0.56f, 0.56f, 0.54f, 1.0f);
        snprintf(buf, sizeof(buf), "%d ms", p->pingMs);
        Nu_DrawTextRight(buf, x + w - pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
        ty += rowH;
    }

    if (count > shown)
    {
        snprintf(buf, sizeof(buf), "and %d more", count - shown);
        Nu_DrawText(buf, x + pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
        ty += rowH;
    }

    /* The Steam session, when there is one. It is a different thing from the
     * roster above -- the people you invited, not everyone who happens to be
     * on the same master server -- so it gets its own block rather than being
     * mixed into the list. */
    if (ShSession_MemberCount() > 0)
    {
        char sline[128];
        int  n = ShSession_MemberCount();
        int  k;

        ty += rowH * 0.5f;
        {
            const float x0 = (x + pad) / s_vpW * 2.0f - 1.0f;
            const float x1 = (x + w - pad) / s_vpW * 2.0f - 1.0f;
            const float yy = 1.0f - (ty / s_vpH) * 2.0f;
            Nu_Fill(x0, yy, x1, yy - 2.0f / s_vpH, 0.4f, 0.39f, 0.36f, 0.5f);
        }
        ty += rowH * 0.4f;

        ShSession_StatusLine(sline, (int)sizeof(sline));
        Nu_DrawText(sline, x + pad, ty, px, 0.62f, 0.66f, 0.58f, 1.0f);
        ty += rowH;

        for (k = 0; k < n; k++)
        {
            const ShSessionMember* m = ShSession_Member(k);
            if (!m)
            {
                continue;
            }
            Nu_DrawText(m->name, x + pad + (float)px, ty, px,
                        m->linked ? 0.78f : 0.46f,
                        m->linked ? 0.76f : 0.45f,
                        m->linked ? 0.62f : 0.43f, 1.0f);
            if (m->mapIdx >= 0 && m->mapIdx < 64)
            {
                Nu_DrawText(Pc_MapAreaName(m->mapIdx), x + w * 0.38f, ty, px,
                            0.52f, 0.52f, 0.50f, 1.0f);
            }
            if (m->pingMs >= 0)
            {
                snprintf(buf, sizeof(buf), "%d ms", m->pingMs);
            }
            else
            {
                snprintf(buf, sizeof(buf), "%s", m->linked ? "--" : "no link");
            }
            Nu_DrawTextRight(buf, x + w - pad, ty, px, 0.45f, 0.45f, 0.43f, 1.0f);
            ty += rowH;
        }
    }

    ty = y + h - pad - (float)px;
    Nu_DrawText("F11 close     M leave a message", x + pad, ty, px,
                0.42f, 0.42f, 0.40f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Memo composer                                                       */
/* ------------------------------------------------------------------ */

static void Nu_DrawComposer(int px)
{
    const float pad  = (float)px * 0.9f;
    const float rowH = (float)px * 1.4f;
    const float w    = s_vpW * 0.46f < 460.0f ? 460.0f : s_vpW * 0.46f;
    const float h    = pad * 2.0f + rowH * (float)(NU_COMPOSER_ROWS + 4);
    const float x    = (s_vpW - w) * 0.5f;
    const float y    = s_vpH * 0.16f;
    const int   col  = ShNetMemo_ComposerColumn();
    /* Three columns: phrase | word | emote. The emote column is narrow. */
    const float usable = w - pad * 4.0f;
    const float colWs[3] = { usable * 0.46f, usable * 0.34f, usable * 0.20f };
    float       ty;
    int         i;
    char        buf[128];

    Nu_Panel(x, y, w, h, 1.0f);

    ty = y + pad;
    Nu_DrawText("LEAVE A MESSAGE", x + pad, ty, px, 0.82f, 0.79f, 0.70f, 1.0f);
    ty += rowH * 1.3f;

    for (i = 0; i < 3; i++)
    {
        const int   isPhrase = (i == 0);
        const int   isEmote  = (i == 2);
        const int   sel      = isPhrase ? ShNetMemo_ComposerPhrase()
                             : isEmote  ? ShNetMemo_ComposerEmote() : ShNetMemo_ComposerWord();
        const int   total    = isPhrase ? ShNetMemo_PhraseCount()
                             : isEmote  ? ShNetMemo_EmoteCount()    : ShNetMemo_WordCount();
        const float colW     = colWs[i];
        const float cx       = x + pad + (i > 0 ? colWs[0] + pad : 0.0f) + (i > 1 ? colWs[1] + pad : 0.0f);
        const int   half     = NU_COMPOSER_ROWS / 2;
        float       cy       = ty;
        int         k;

        if (col == i)
        {
            const float x0 = (cx - pad * 0.4f) / s_vpW * 2.0f - 1.0f;
            const float x1 = (cx + colW) / s_vpW * 2.0f - 1.0f;
            const float yT = 1.0f - ((cy - pad * 0.3f) / s_vpH) * 2.0f;
            const float yB = 1.0f - ((cy + rowH * (float)NU_COMPOSER_ROWS) / s_vpH) * 2.0f;
            Nu_Fill(x0, yT, x1, yB, 0.16f, 0.16f, 0.14f, 0.55f);
        }

        for (k = -half; k <= half; k++)
        {
            /* Wrap the visible window so the list reads as a wheel: the entry
             * above the first is the last, which is how it navigates. */
            int idx = ((sel + k) % total + total) % total;
            int on  = (k == 0);
            const char* label = isPhrase ? ShNetMemo_Phrase(idx)
                              : isEmote  ? (idx == 0 ? "(no emote)" : ShNetMemo_Emote(idx))
                                         : ShNetMemo_Word(idx);

            if (isEmote)
            {
                /* plain tags, nothing to substitute or group */
            }
            else if (isPhrase)
            {
                /* Show the template with its slot filled by a placeholder, so
                 * the player reads a sentence rather than a printf string. */
                const char* p = label;
                int         o = 0;
                while (*p && o < (int)sizeof(buf) - 8)
                {
                    if (p[0] == '%' && p[1] == 's')
                    {
                        buf[o++] = '_'; buf[o++] = '_'; buf[o++] = '_';
                        p += 2;
                        continue;
                    }
                    buf[o++] = *p++;
                }
                buf[o] = '\0';
                label  = buf;
            }
            else
            {
                const char* grp = ShNetMemo_WordGroup(idx);
                if (grp && !on)
                {
                    Nu_DrawTextRight(grp, cx + colW, cy, (int)((float)px * 0.8f),
                                     0.38f, 0.42f, 0.38f, 1.0f);
                }
            }

            Nu_DrawText(label, cx + (on ? (float)px * 0.5f : 0.0f), cy, px,
                        on ? 0.92f : 0.46f,
                        on ? 0.88f : 0.45f,
                        on ? 0.72f : 0.43f, on ? 1.0f : 0.85f);
            cy += rowH;
        }
    }

    ty += rowH * (float)NU_COMPOSER_ROWS + rowH * 0.4f;
    ShNetMemo_ComposerPreview(buf, (int)sizeof(buf));
    Nu_DrawTextCentered(buf, x + w * 0.5f, ty, (int)((float)px * 1.3f),
                        0.88f, 0.84f, 0.62f, 1.0f);
    ty += rowH * 1.4f;
    Nu_DrawTextCentered(col == 0 ? "Up/Down choose     Enter next     Cancel (X) close"
                      : col == 1 ? "Up/Down word     Left/Right column     Enter next     Cancel (X) close"
                                 : "Up/Down emote     Left back     Enter leave it     Cancel (X) close",
                        x + w * 0.5f, ty, (int)((float)px * 0.9f),
                        0.42f, 0.42f, 0.40f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* The marker under your feet                                          */
/* ------------------------------------------------------------------ */

static void Nu_DrawNearbyMemo(int px)
{
    const int        idx = ShNetMemo_NearestReadable();
    const ShNetMemo* m;
    char             text[128];
    char             sub[128];
    const int        big = (int)((float)px * 1.4f);
    float            y;

    if (idx < 0)
    {
        return;
    }
    m = ShNet_Memo(idx);
    if (!m)
    {
        return;
    }

    ShNetMemo_Text(m, text, (int)sizeof(text));
    y = s_vpH * 0.80f;

    {
        const float tw = Nu_TextWidth(text, big);
        const float bx = (s_vpW - tw) * 0.5f - (float)px;
        Nu_Panel(bx, y - (float)px * 0.5f, tw + (float)px * 2.0f, (float)big * 2.6f, 0.85f);
    }

    {
        const int kind = m->kind & 0x0F; /* high nibble = emote */
        Nu_DrawTextCentered(text, s_vpW * 0.5f, y, big,
                            kind == SHNET_MARK_DEATH ? 0.80f : 0.88f,
                            kind == SHNET_MARK_DEATH ? 0.42f : 0.84f,
                            kind == SHNET_MARK_DEATH ? 0.40f : 0.62f, 1.0f);
    }

    if ((m->kind & 0x0F) == SHNET_MARK_MEMO)
    {
        snprintf(sub, sizeof(sub), "- %s   (%+d)", m->owner[0] ? m->owner : "someone",
                 (int)m->rating);
    }
    else
    {
        snprintf(sub, sizeof(sub), "%s", "");
    }
    if (sub[0])
    {
        Nu_DrawTextCentered(sub, s_vpW * 0.5f, y + (float)big * 1.25f,
                            (int)((float)px * 0.9f), 0.48f, 0.48f, 0.46f, 1.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

void ShNetUi_PreloadGL(void)
{
    if (s_glReady == 0)
    {
        Nu_GlInit();
        Nu_FontInit();
    }
}

void ShNetUi_TogglePlayerList(void)
{
    if (!g_PcConfig.onlineEnabled && !g_PcConfig.onlineSteam)
    {
        return;
    }
    g_ShNetPlayerListOpen = !g_ShNetPlayerListOpen;
    if (g_ShNetPlayerListOpen)
    {
        ShNet_RequestRoster();
    }
}

static float Nu_DrawTextFit(const char* text, float x, float y, float maxW, int px,
                            float r, float g, float b, float a);

static void Nu_DrawChat(int px)
{
    const int   n    = ShNetChat_LineCount();
    const int   comp = ShNetChat_Composing();
    const float lh   = (float)px * 1.32f;
    const float x    = s_vpH * 0.03f;
    const float bot  = s_vpH * (comp ? 0.9f : 0.93f);
    float       y;
    int         i;

    if (n <= 0 && !comp)
    {
        return;
    }

    /* Backing slab only while composing -- idle lines float over the scene the
     * way a console-style feed does, so they never box off the corner. */
    if (comp)
    {
        float top = bot - lh * (float)(n + 1) - (float)px * 0.5f;
        Nu_Panel(x - (float)px * 0.5f, top, s_vpW * 0.62f, bot - top + lh,
                 0.85f);
    }

    y = bot - lh * (float)(n + (comp ? 1 : 0));
    for (i = 0; i < n; i++)
    {
        float a = ShNetChat_LineAlpha(i);
        int   g = (ShNetChat_LineScope(i) == SHNET_CHAT_GAME);
        if (a <= 0.0f)
        {
            y += lh;
            continue;
        }
        Nu_DrawText(ShNetChat_LineText(i), x, y, px,
                    g ? 0.86f : 0.72f,
                    g ? 0.80f : 0.78f,
                    g ? 0.62f : 0.86f, a);
        y += lh;
    }

    if (comp)
    {
        char        line[200];
        const int   g = (ShNetChat_Scope() == SHNET_CHAT_GAME);
        snprintf(line, sizeof(line), "%s %s_",
                 g ? "[Game]" : "[All]", ShNetChat_ComposeText());
        Nu_DrawTextFit(line, x, y, s_vpW * 0.60f, px,
                       g ? 0.95f : 0.80f, g ? 0.90f : 0.86f, g ? 0.66f : 0.95f, 1.0f);
    }
}

/* Filled rect in viewport pixels (the selected-row highlight). */
static void Nu_FillPx(float x, float y, float w, float h,
                      float r, float g, float b, float a)
{
    const float x0 = (x / s_vpW) * 2.0f - 1.0f;
    const float x1 = ((x + w) / s_vpW) * 2.0f - 1.0f;
    const float yT = 1.0f - (y / s_vpH) * 2.0f;
    const float yB = 1.0f - ((y + h) / s_vpH) * 2.0f;
    Nu_Fill(x0, yT, x1, yB, r, g, b, a);
}

/* A plain white arrow with a black outline, generated once and drawn in the
 * overlay above the modal panel -- the game's own cursor is painted into the PSX
 * frame underneath and so is hidden by the panel. Generated rather than read
 * from VRAM (the earlier sprite path drew nothing on the title screen). The
 * hotspot is the tip at the top-left, placed at the pointer. */
#define NU_CUR_W 20
#define NU_CUR_H 28
static void Nu_DrawCursor(void)
{
    static unsigned char s_cur[NU_CUR_W * NU_CUR_H * 4];
    static int           s_curBuilt = 0;
    float  nx, ny, x, y, cw, ch;
    GLuint slot;

    if (!Pc_MouseCursor_ViewportPos(&nx, &ny))
    {
        return;
    }

    if (!s_curBuilt)
    {
        unsigned char inside[NU_CUR_W * NU_CUR_H];
        /* Arrowhead triangle: tip A(0,0), straight down the left edge to
         * B(0,H-1), across to the point C(0.62W, 0.62H). */
        const float ax = 0, ay = 0, bx = 0, by = NU_CUR_H - 1;
        const float ccx = NU_CUR_W * 0.62f, ccy = NU_CUR_H * 0.62f;
        int px, py, i, dx, dy;

        for (py = 0; py < NU_CUR_H; py++)
            for (px = 0; px < NU_CUR_W; px++)
            {
                float p = (float)px, q = (float)py;
                float d1 = (p - bx)  * (ay - by)  - (ax - bx)  * (q - by);
                float d2 = (p - ccx) * (by - ccy) - (bx - ccx) * (q - ccy);
                float d3 = (p - ax)  * (ccy - ay) - (ccx - ax) * (q - ay);
                int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                inside[py * NU_CUR_W + px] = (unsigned char)(!(neg && pos));
            }
        for (i = 0; i < NU_CUR_W * NU_CUR_H * 4; i++) s_cur[i] = 0;
        for (py = 0; py < NU_CUR_H; py++)
            for (px = 0; px < NU_CUR_W; px++)
            {
                int o = (py * NU_CUR_W + px) * 4;
                if (inside[py * NU_CUR_W + px])
                {
                    s_cur[o] = s_cur[o + 1] = s_cur[o + 2] = 255; s_cur[o + 3] = 255;
                }
                else
                {
                    int edge = 0;
                    for (dy = -1; dy <= 1 && !edge; dy++)
                        for (dx = -1; dx <= 1; dx++)
                        {
                            int qx = px + dx, qy = py + dy;
                            if (qx >= 0 && qx < NU_CUR_W && qy >= 0 && qy < NU_CUR_H &&
                                inside[qy * NU_CUR_W + qx]) { edge = 1; break; }
                        }
                    if (edge) { s_cur[o] = s_cur[o + 1] = s_cur[o + 2] = 0; s_cur[o + 3] = 255; }
                }
            }
        s_curBuilt = 1;
    }

    /* Upload once into a permanent slot; reuse the handle every frame. Uploading
     * per frame leaked a slot per frame and exhausted the atlas. */
    if (!s_curSlot)
    {
        s_curSlot = Nu_Upload(s_cur, NU_CUR_W, NU_CUR_H);
    }
    slot = s_curSlot;
    if (!slot)
    {
        return;
    }
    ch = s_vpH * 0.055f;
    if (ch < 18.0f) ch = 18.0f;
    cw = ch * ((float)NU_CUR_W / (float)NU_CUR_H);
    x  = nx * s_vpW; /* tip (hotspot) at the pointer */
    y  = ny * s_vpH;
    Nu_Quad(slot,
            (x / s_vpW) * 2.0f - 1.0f, 1.0f - (y / s_vpH) * 2.0f,
            ((x + cw) / s_vpW) * 2.0f - 1.0f, 1.0f - ((y + ch) / s_vpH) * 2.0f,
            1.0f, 1.0f, 1.0f, 1.0f);
}

/* ---- Silent Hill styled co-op front end ----------------------------------
 * The game's own menus (inventory, save dialog) frame things with two nested
 * one-pixel boxes in blue (96,96,255) outside and (32,32,255) inside, and
 * light a selection with a blue gradient that fades into the dark. The layout
 * is the classic PC server-browser one: a framed page with a header, a body
 * of rows or a column table, buttons along the bottom and a key-hint bar. */

#define NU_SH_BLUE_HI_R 0.376f
#define NU_SH_BLUE_HI_G 0.376f
#define NU_SH_BLUE_LO_R 0.125f
#define NU_SH_BLUE_LO_G 0.125f

typedef struct { float x, y, w, h; } NuRect;

static void Nu_LinePx(float x, float y, float w, float h, float r, float g, float b, float a)
{
    Nu_FillPx(x, y, w, h, r, g, b, a);
}

static void Nu_BoxPx(float x, float y, float w, float h, float t, float r, float g, float b, float a)
{
    Nu_LinePx(x, y, w, t, r, g, b, a);
    Nu_LinePx(x, y + h - t, w, t, r, g, b, a);
    Nu_LinePx(x, y + t, t, h - 2.0f * t, r, g, b, a);
    Nu_LinePx(x + w - t, y + t, t, h - 2.0f * t, r, g, b, a);
}

/* The inventory's double frame. */
static void Nu_ShFrame(float x, float y, float w, float h, float t, float a)
{
    Nu_BoxPx(x, y, w, h, t, NU_SH_BLUE_HI_R, NU_SH_BLUE_HI_G, 1.0f, a);
    Nu_BoxPx(x + 2.0f * t, y + 2.0f * t, w - 4.0f * t, h - 4.0f * t, t,
             NU_SH_BLUE_LO_R, NU_SH_BLUE_LO_G, 1.0f, a);
}

/* Horizontal fade: the shader takes one colour per quad, so the gradient is
 * a run of thin strips. */
static void Nu_HFade(float x, float y, float w, float h,
                     float r, float g, float b, float a0, float a1)
{
    const int strips = 24;
    int       i;
    for (i = 0; i < strips; i++)
    {
        float t  = ((float)i + 0.5f) / (float)strips;
        float sx = (float)(int)(x + w * (float)i / (float)strips + 0.5f);
        float ex = (float)(int)(x + w * (float)(i + 1) / (float)strips + 0.5f);
        Nu_FillPx(sx, y, ex - sx, h, r, g, b, a0 + (a1 - a0) * t);
    }
}

static void Nu_VFade(float x, float y, float w, float h,
                     float r, float g, float b, float a0, float a1)
{
    const int strips = 24;
    int       i;
    for (i = 0; i < strips; i++)
    {
        float t  = ((float)i + 0.5f) / (float)strips;
        float sy = (float)(int)(y + h * (float)i / (float)strips + 0.5f);
        float ey = (float)(int)(y + h * (float)(i + 1) / (float)strips + 0.5f);
        Nu_FillPx(x, sy, w, ey - sy, r, g, b, a0 + (a1 - a0) * t);
    }
}

static float Nu_TextH(int px)
{
    int w = 0, h = 0;
    return Nu_Text("Ag", px, &w, &h) ? (float)h : (float)px;
}

/* Text clipped to a width by dropping characters off the end. */
static float Nu_DrawTextFit(const char* text, float x, float y, float maxW, int px,
                           float r, float g, float b, float a)
{
    char buf[128];
    int  n;
    if (Nu_TextWidth(text, px) <= maxW)
    {
        return Nu_DrawText(text, x, y, px, r, g, b, a);
    }
    SDL_strlcpy(buf, text, sizeof(buf));
    for (n = (int)strlen(buf); n > 1; n--)
    {
        buf[n - 1] = '\0';
        buf[n - 2] = '.';
        if (Nu_TextWidth(buf, px) <= maxW) break;
    }
    return Nu_DrawText(buf, x, y, px, r, g, b, a);
}

/* A row's "Label:  Value" text, split for two-column drawing. */
static int Nu_SplitRow(const char* row, char* label, int lcap, char* value, int vcap)
{
    const char* sep = strstr(row, ":  ");
    if (!sep)
    {
        SDL_strlcpy(label, row, (size_t)lcap);
        value[0] = '\0';
        return 0;
    }
    {
        int n = (int)(sep - row);
        if (n >= lcap) n = lcap - 1;
        memcpy(label, row, (size_t)n);
        label[n] = '\0';
    }
    sep += 3;
    while (*sep == ' ') sep++;
    SDL_strlcpy(value, sep, (size_t)vcap);
    return 1;
}

/* Rows that leave a page rather than act on it go on the button bar. */
static int Nu_IsFooterRow(const char* row)
{
    return strcmp(row, "Back") == 0 || strcmp(row, "Refresh") == 0;
}

/* One line beside a plain menu choice, for a player who has never hosted. */
static const char* Nu_RowHint(const char* row)
{
    if (strcmp(row, "Host Game") == 0)             return "Open a room for friends or anyone";
    if (strcmp(row, "Join Game") == 0)             return "Find an open room, or use a code";
    if (strcmp(row, "Browse Server Rooms") == 0)   return "Public rooms on the community server";
    if (strcmp(row, "Join by IP...") == 0)         return "Connect straight to a friend's PC";
    if (strcmp(row, "Enter Room Code...") == 0)    return "The four digits a host shared";
    if (strcmp(row, "Join a Friend (Steam)") == 0) return "Through a Steam invite";
    return NULL;
}

#define NU_COOP_MAX_ROWS 48

static void Nu_DrawCoopMenu(int px)
{
    const int   n        = Pc_CoopMenu_RowCount() < NU_COOP_MAX_ROWS ? Pc_CoopMenu_RowCount() : NU_COOP_MAX_ROWS;
    const int   view     = Pc_CoopMenu_View();
    const int   editing  = Pc_CoopMenu_Editing();
    const int   sel      = Pc_CoopMenu_Selected();
    const int   pxSmall  = (int)((float)px * 0.74f);
    const int   pxTitle  = (int)((float)px * 1.30f);
    /* The PSX frame is one pixel at 240 lines; scaled that far it reads as a
     * slab, so it grows at half that rate. */
    const float t        = (s_vpH / 480.0f) < 1.0f ? 1.0f : (float)(int)(s_vpH / 480.0f + 0.5f);
    const float pad      = (float)px * 1.15f;
    const float rowH     = (float)px * 1.85f;
    const float btnH     = (float)px * 1.75f;
    char        rowText[NU_COOP_MAX_ROWS][96];
    int         footer[NU_COOP_MAX_ROWS];
    NuRect      rect[NU_COOP_MAX_ROWS];
    int         bodyIdx[NU_COOP_MAX_ROWS], bodyN = 0;
    int         footIdx[NU_COOP_MAX_ROWS], footN = 0;
    char        status[160];
    float       panelW, panelH, panelX, panelY, headerH, bodyH, statusH, footerH, hintH;
    float       bodyY, x0, x1, y;
    int         i;

    for (i = 0; i < n; i++)
    {
        Pc_CoopMenu_RowText(i, rowText[i], (int)sizeof(rowText[i]));
        footer[i] = Nu_IsFooterRow(rowText[i]);
        if (footer[i]) footIdx[footN++] = i;
        else           bodyIdx[bodyN++] = i;
    }
    Pc_CoopMenu_StatusText(status, sizeof(status));

    /* ---- geometry ---- */
    panelW = s_vpW * (view == PC_COOP_VIEW_ROOMS ? 0.62f : 0.54f);
    if (panelW < 440.0f) panelW = (s_vpW - 32.0f < 440.0f) ? s_vpW - 32.0f : 440.0f;
    if (panelW > 980.0f) panelW = 980.0f;
    headerH = pad * 0.8f + Nu_TextH(pxSmall) + (float)pxTitle * 1.15f + pad * 0.35f;
    bodyH   = rowH * (float)(bodyN > 0 ? bodyN : 0);
    if (view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS)
    {
        float minRows = 6.0f;
        bodyH += rowH * 0.9f; /* column header */
        if (bodyH < rowH * minRows) bodyH = rowH * minRows;
    }
    statusH = (status[0] || editing) ? (editing ? Nu_TextH(pxSmall) + rowH * 1.25f : Nu_TextH(pxSmall) + pad * 0.7f) : 0.0f;
    footerH = footN ? btnH + pad * 0.8f : 0.0f;
    hintH   = Nu_TextH(pxSmall) + pad * 0.7f;
    panelH  = headerH + pad * 0.5f + bodyH + pad * 0.5f + statusH + footerH + hintH;
    if (panelH > s_vpH - 24.0f) panelH = s_vpH - 24.0f;
    panelX = (s_vpW - panelW) * 0.5f;
    panelY = (s_vpH - panelH) * 0.5f;
    x0     = panelX + pad;
    x1     = panelX + panelW - pad;
    bodyY  = panelY + headerH + pad * 0.5f;

    /* Row rectangles, shared by hit-testing and drawing so they cannot drift. */
    y = bodyY + ((view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS) ? rowH * 0.9f : 0.0f);
    for (i = 0; i < bodyN; i++)
    {
        rect[bodyIdx[i]].x = x0;
        rect[bodyIdx[i]].y = y;
        rect[bodyIdx[i]].w = x1 - x0;
        rect[bodyIdx[i]].h = rowH;
        y += rowH;
    }
    {
        const float bw = (float)px * 7.5f;
        float       bx = x1;
        const float by = bodyY + bodyH + pad * 0.5f + statusH + pad * 0.4f;
        for (i = footN - 1; i >= 0; i--)
        {
            bx -= bw;
            rect[footIdx[i]].x = bx;
            rect[footIdx[i]].y = by;
            rect[footIdx[i]].w = bw;
            rect[footIdx[i]].h = btnH;
            bx -= (float)px * 0.6f;
        }
    }

    /* ---- pointer ---- */
    {
        float mnx, mny;
        int   hov = -1;
        if (!editing && Pc_MouseCursor_ViewportPos(&mnx, &mny))
        {
            const float mx = mnx * s_vpW, my = mny * s_vpH;
            for (i = 0; i < n; i++)
            {
                if (mx >= rect[i].x && mx < rect[i].x + rect[i].w &&
                    my >= rect[i].y && my < rect[i].y + rect[i].h)
                {
                    hov = i;
                    break;
                }
            }
        }
        Pc_CoopMenu_SetHover(hov);
    }

    /* ---- backdrop and panel ---- */
    Nu_FillPx(0.0f, 0.0f, s_vpW, s_vpH, 0.0f, 0.0f, 0.0f, 0.55f);
    Nu_FillPx(panelX, panelY, panelW, panelH, 0.0f, 0.0f, 0.02f, 0.88f);
    Nu_VFade(panelX, panelY, panelW, headerH, 0.05f, 0.08f, 0.30f, 0.55f, 0.0f);
    Nu_ShFrame(panelX, panelY, panelW, panelH, t, 1.0f);

    /* ---- header ---- */
    {
        const float hy = panelY + pad * 0.8f;
        const char* title = Pc_CoopMenu_Title();
        Nu_DrawText(Pc_CoopMenu_IsTopLevel() ? "SILENT HILL  ONLINE" : "MULTIPLAYER", x0, hy, pxSmall,
                    0.55f, 0.60f, 1.00f, 0.95f);
        Nu_DrawText(title, x0, hy + Nu_TextH(pxSmall), pxTitle, 0.92f, 0.91f, 0.88f, 1.0f);
        Nu_LinePx(x0, panelY + headerH, x1 - x0, t, NU_SH_BLUE_HI_R, NU_SH_BLUE_HI_G, 1.0f, 0.9f);
        Nu_LinePx(x0, panelY + headerH + 2.0f * t, (x1 - x0) * 0.6f, t, NU_SH_BLUE_LO_R, NU_SH_BLUE_LO_G, 1.0f, 0.9f);
    }

    /* ---- column headers ---- */
    if (view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS)
    {
        const float cy = bodyY + (rowH * 0.9f - Nu_TextH(pxSmall)) * 0.5f;
        const float w  = x1 - x0;
        if (view == PC_COOP_VIEW_ROOMS)
        {
            Nu_DrawText("GAME", x0 + px * 0.8f, cy, pxSmall, 0.55f, 0.60f, 1.0f, 0.9f);
            Nu_DrawText("WHERE", x0 + w * 0.46f, cy, pxSmall, 0.55f, 0.60f, 1.0f, 0.9f);
            Nu_DrawTextRight("PLAYERS", x1 - px * 0.8f, cy, pxSmall, 0.55f, 0.60f, 1.0f, 0.9f);
        }
        else
        {
            Nu_DrawText("PLAYER", x0 + px * 0.8f, cy, pxSmall, 0.55f, 0.60f, 1.0f, 0.9f);
            Nu_DrawTextRight("PING", x1 - px * 0.8f, cy, pxSmall, 0.55f, 0.60f, 1.0f, 0.9f);
        }
        Nu_LinePx(x0, bodyY + rowH * 0.9f - t, w, t, NU_SH_BLUE_LO_R, NU_SH_BLUE_LO_G, 1.0f, 0.8f);
    }

    /* ---- body rows ---- */
    {
        const int roomCount   = (view == PC_COOP_VIEW_ROOMS) ? ShSession_RoomCount() : 0;
        const int memberCount = (view == PC_COOP_VIEW_PLAYERS) ? ShSession_MemberCount() : 0;
        const float textH     = Nu_TextH(px);

        if (view == PC_COOP_VIEW_ROOMS && roomCount == 0)
        {
            Nu_DrawTextCentered("No open rooms yet.  Refresh, or host one yourself.",
                                panelX + panelW * 0.5f, bodyY + bodyH * 0.45f, px,
                                0.55f, 0.56f, 0.62f, 0.9f);
        }
        for (i = 0; i < bodyN; i++)
        {
            const int    k  = bodyIdx[i];
            const NuRect r  = rect[k];
            const int    on = (k == sel) && !editing;
            const float  ty = r.y + (r.h - textH) * 0.5f;
            const float  lt = on ? 1.00f : 0.74f;

            if (on)
            {
                Nu_HFade(r.x, r.y, r.w, r.h, 0.0f, 0.42f, 0.75f, 0.55f, 0.0f);
                Nu_ShFrame(r.x, r.y, r.w, r.h, t, 0.9f);
            }
            else if (view == PC_COOP_VIEW_ROOMS || view == PC_COOP_VIEW_PLAYERS)
            {
                if (i & 1) Nu_FillPx(r.x, r.y, r.w, r.h, 0.10f, 0.10f, 0.30f, 0.18f);
            }

            if (view == PC_COOP_VIEW_ROOMS && k < roomCount)
            {
                int  code = 0, players = 0, max = 0, map = -1;
                char host[SHSESSION_NAME_MAX];
                char cell[96];
                host[0] = '\0';
                ShSession_RoomGet(k, &code, &players, &max, &map, host, (int)sizeof(host));
                /* "Name's Game", the room code (or Steam) small beside it. */
                snprintf(cell, sizeof(cell), "%s's Game", host[0] ? host : "Someone");
                {
                    float nw = Nu_DrawTextFit(cell, r.x + px * 0.8f, ty, r.w * 0.36f, px, lt, lt, lt, 1.0f);
                    if (ShSession_RoomLobby(k)) snprintf(cell, sizeof(cell), "Steam");
                    else                        snprintf(cell, sizeof(cell), "#%d", code);
                    Nu_DrawText(cell, r.x + px * 1.3f + nw, ty + (textH - Nu_TextH(pxSmall)) * 0.5f,
                                pxSmall, 0.55f, 0.60f, 1.0f, 0.85f);
                }
                Nu_DrawTextFit(map >= 0 ? Pc_MapAreaName(map) : "In the lobby",
                               r.x + r.w * 0.46f, ty, r.w * 0.36f, px, lt * 0.92f, lt * 0.92f, lt, 1.0f);
                snprintf(cell, sizeof(cell), "%d / %d", players, max);
                Nu_DrawTextRight(cell, r.x + r.w - px * 0.8f, ty, px,
                                 players >= max ? 0.75f : 0.62f, players >= max ? 0.45f : 0.72f,
                                 players >= max ? 0.45f : 1.00f, 1.0f);
            }
            else if (view == PC_COOP_VIEW_PLAYERS && k < memberCount)
            {
                const ShSessionMember* m = ShSession_Member(k);
                char                   cell[32];
                if (m)
                {
                    float nw = Nu_DrawText(m->name, r.x + px * 0.8f, ty, px, lt, lt, lt, 1.0f);
                    if (m->steamId == ShSession_HostId())
                    {
                        Nu_DrawText("HOST", r.x + px * 1.4f + nw, ty + (textH - Nu_TextH(pxSmall)) * 0.5f,
                                    pxSmall, 0.55f, 0.60f, 1.0f, 0.95f);
                    }
                    if (m->pingMs >= 0) snprintf(cell, sizeof(cell), "%d ms", m->pingMs);
                    else                snprintf(cell, sizeof(cell), "-");
                    Nu_DrawTextRight(cell, r.x + r.w - px * 0.8f, ty, px, 0.62f, 0.72f, 1.0f, 1.0f);
                }
            }
            else
            {
                char label[96], value[96];
                if (Nu_SplitRow(rowText[k], label, (int)sizeof(label), value, (int)sizeof(value)))
                {
                    Nu_DrawText(label, r.x + px * 0.8f, ty, px, lt, lt, lt, 1.0f);
                    Nu_DrawTextRight(value, r.x + r.w - px * 0.8f, ty, px,
                                     on ? 1.0f : 0.62f, on ? 1.0f : 0.72f, 1.0f, 1.0f);
                }
                else
                {
                    const char* hint = (view == PC_COOP_VIEW_MENU) ? Nu_RowHint(label) : NULL;
                    Nu_DrawText(label, r.x + px * 0.8f, ty, px, lt, lt, lt, 1.0f);
                    if (hint)
                    {
                        Nu_DrawTextRight(hint, r.x + r.w - px * 0.8f, ty + (textH - Nu_TextH(pxSmall)) * 0.5f,
                                         pxSmall, on ? 0.70f : 0.45f, on ? 0.76f : 0.48f, on ? 1.0f : 0.62f, 1.0f);
                    }
                }
            }
        }
    }

    /* ---- status / text entry ---- */
    if (statusH > 0.0f)
    {
        const float sy = bodyY + bodyH + pad * 0.5f;
        Nu_LinePx(x0, sy - pad * 0.25f, x1 - x0, t, NU_SH_BLUE_LO_R, NU_SH_BLUE_LO_G, 1.0f, 0.6f);
        if (editing)
        {
            /* The model's line is "Label:  typed_   [keys]": label above the
             * field, the typed text inside it, the keys on the hint bar. */
            char        label[96], value[96];
            const float fh = rowH;
            const float fy = sy + Nu_TextH(pxSmall) + pad * 0.1f;
            char*       keys;
            Nu_SplitRow(status, label, (int)sizeof(label), value, (int)sizeof(value));
            keys = strstr(value, "   [");
            if (keys) *keys = '\0';
            Nu_DrawText(label, x0, sy, pxSmall, 0.55f, 0.60f, 1.0f, 0.95f);
            Nu_FillPx(x0, fy, x1 - x0, fh, 0.0f, 0.0f, 0.0f, 0.75f);
            Nu_ShFrame(x0, fy, x1 - x0, fh, t, 1.0f);
            Nu_DrawTextFit(value, x0 + px * 0.8f, fy + (fh - Nu_TextH(px)) * 0.5f, x1 - x0 - px * 1.6f, px,
                           1.0f, 1.0f, 1.0f, 1.0f);
        }
        else
        {
            Nu_DrawTextFit(status, x0, sy + pad * 0.2f, x1 - x0, pxSmall, 0.78f, 0.76f, 0.70f, 0.95f);
        }
    }

    /* ---- buttons ---- */
    for (i = 0; i < footN; i++)
    {
        const int    k  = footIdx[i];
        const NuRect r  = rect[k];
        const int    on = (k == sel) && !editing;
        Nu_FillPx(r.x, r.y, r.w, r.h, 0.0f, 0.0f, 0.05f, 0.85f);
        if (on) Nu_VFade(r.x, r.y, r.w, r.h, 0.0f, 0.42f, 0.75f, 0.10f, 0.55f);
        Nu_ShFrame(r.x, r.y, r.w, r.h, t, on ? 1.0f : 0.55f);
        Nu_DrawTextCentered(rowText[k], r.x + r.w * 0.5f, r.y + (r.h - Nu_TextH(px)) * 0.5f, px,
                            on ? 1.0f : 0.74f, on ? 1.0f : 0.74f, on ? 1.0f : 0.78f, 1.0f);
    }

    /* ---- key hints ---- */
    {
        const char* hint = editing ? "Type    [Enter]  Confirm    [Esc]  Cancel"
                         : Pc_CoopMenu_IsTopLevel() ? "[Up/Down]  Select    [Enter] or Click  Choose    [Esc]  Close"
                                                    : "[Up/Down]  Select    [Enter] or Click  Choose    [Esc]  Back";
        Nu_DrawText(hint, x0, panelY + panelH - hintH + pad * 0.15f, pxSmall, 0.45f, 0.47f, 0.60f, 0.95f);
    }

    Nu_DrawCursor();
}

void ShNetUi_Draw(void)
{
    GLint     vp[4];
    GLint     prevProg, prevTex, prevVao, prevBuf, prevActive;
    GLint     prevSrcRgb, prevDstRgb, prevSrcA, prevDstA, prevEqRgb, prevEqA;
    GLboolean prevDepth, prevBlend, prevCull, prevScissor;
    int       px;
    int       drawList, drawComposer, drawStatus;

    if (!ShNet_Enabled() && !Pc_CoopMenu_IsOpen())
    {
        return;
    }
    if (s_glReady == 0)
    {
        ShNetUi_PreloadGL();
    }
    if (s_glReady != 1)
    {
        return;
    }

    /* Atlas ran dry last frame: reset it and drop every cached handle so this
     * frame rebuilds from an empty atlas. Costs one frame, always recovers --
     * the alternative was text silently vanishing for the rest of the session. */
    if (s_atlasExhausted)
    {
        Nu_AtlasReset();
        memset(s_cache, 0, sizeof(s_cache));
        s_atlasExhausted = 0;
    }

    Nu_PumpEvents();

    drawList     = g_ShNetPlayerListOpen;
    drawComposer = ShNetMemo_ComposerActive();
    /* SEAMLESS BY DESIGN. The living world is not something you connect to,
     * it is something you are in, so the normal path -- resolving, connecting,
     * reconnecting after a blip -- says NOTHING. Two exceptions, both cases
     * where the player has to actually do something:
     *
     *   REJECTED  the server refused us (wrong password, wrong version, full).
     *             It will never resolve itself.
     *   LOST      but only once it has stayed lost. A dropped packet or a map
     *             load should not put a banner on screen; a server that has
     *             genuinely gone away for a quarter of a minute should.
     *
     * F11 always tells the truth for anyone who wants to look. */
    {
        const int st = ShNet_Status();
        if (st == SHNET_ST_LOST)
        {
            if (s_lostSinceMs == 0)
            {
                s_lostSinceMs = SDL_GetTicks();
            }
        }
        else
        {
            s_lostSinceMs = 0;
        }
        drawStatus = (st == SHNET_ST_REJECTED) ||
                     (st == SHNET_ST_LOST && s_lostSinceMs != 0 &&
                      SDL_GetTicks() - s_lostSinceMs > NU_LOST_QUIET_MS);
    }

    if (!drawList && !drawComposer && !drawStatus && s_eventCount == 0 &&
        ShNetMemo_NearestReadable() < 0 && !ShNetMemo_JustPlaced() &&
        !ShNetChat_DisplayActive() && !Pc_CoopMenu_IsOpen())
    {
        return;
    }
    /* A Steam-session-only build has no master-server status line to show, so
     * the connection banner must not claim one. */
    if (!g_PcConfig.onlineEnabled)
    {
        drawStatus = 0;
    }

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] > 0 && vp[3] > 0)
    {
        s_vpW = (float)vp[2];
        s_vpH = (float)vp[3];
    }
    /* Bigger by default than the original 0.022, and scaled by online_ui_scale
     * (default 1.0) so a player can size the co-op/world overlay text to taste. */
    {
        float sc = (g_PcConfig.onlineUiScale > 0.25f) ? g_PcConfig.onlineUiScale : 1.0f;
        px = (int)(s_vpH * 0.032f * sc);
        if (px < 13) px = 13;
        if (px > 72) px = 72;
    }

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevDepth   = glIsEnabled(GL_DEPTH_TEST);
    prevBlend   = glIsEnabled(GL_BLEND);
    prevCull    = glIsEnabled(GL_CULL_FACE);
    prevScissor = glIsEnabled(GL_SCISSOR_TEST);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* Re-asserted every draw, not just at init: a stale a_uv is the overlay
     * blotch class, and it is invisible in testing because a 1x1 background
     * texture makes every uv sample the same texel. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    /* Explicit equation as well as function: PsyCross leaves the blend
     * EQUATION set to whatever the last world split wanted, and a subtract
     * left behind there is what turns an overlay into a black rectangle. */
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (drawStatus && !drawList)
    {
        char line[160];
        snprintf(line, sizeof(line), "Online: %s", ShNet_StatusText());
        /* Only ever a real problem by the time we get here, so it is allowed
         * to sit in the corner until it is fixed. */
        Nu_DrawText(line, s_vpH * 0.03f, s_vpH * 0.03f, (int)((float)px * 0.9f),
                    0.62f, 0.55f, 0.45f, 0.9f);
    }

    Nu_DrawEvents((int)((float)px * 0.9f));

    if (ShNetMemo_JustPlaced())
    {
        Nu_DrawTextCentered("Message left.", s_vpW * 0.5f, s_vpH * 0.74f, px,
                            0.80f, 0.78f, 0.60f, 1.0f);
    }

    /* Co-op death: no game over, a countdown instead. */
    if (ShNetRespawn_Active())
    {
        const float rem = ShNetRespawn_Remaining();
        const int   big = (int)((float)px * 1.6f);
        char        line[48];
        Nu_DrawTextCentered("YOU DIED", s_vpW * 0.5f, s_vpH * 0.38f, big,
                            0.80f, 0.42f, 0.40f, 1.0f);
        if (rem > 0.05f)
        {
            snprintf(line, sizeof(line), "Respawning in %d", (int)ceilf(rem));
        }
        else
        {
            snprintf(line, sizeof(line), "Respawning...");
        }
        Nu_DrawTextCentered(line, s_vpW * 0.5f, s_vpH * 0.38f + (float)big * 1.3f, px,
                            0.88f, 0.84f, 0.62f, 1.0f);
    }

    /* Co-op: the vocalize wheel while its key is held. */
    if (ShNetVoice_MenuOpen())
    {
        const float cx = s_vpW * 0.5f, cy = s_vpH * 0.5f;
        const float rad = s_vpH * 0.22f;
        const int   sel = ShNetVoice_MenuSelected();
        int         k;
        Nu_FillPx(cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 0.9f, 0.9f, 1.0f, 0.9f);
        for (k = 0; k < 8; k++)
        {
            const float a  = (float)k * 6.2831853f / 8.0f;
            const float lx = cx + (float)sin((double)a) * rad;
            const float ly = cy - (float)cos((double)a) * rad;
            const char* t  = ShNetVoice_MenuLabel(k);
            const float tw = Nu_TextWidth(t, px) + (float)px * 1.2f;
            const float th = Nu_TextH(px) + (float)px * 0.5f;
            const int   on = (k == sel);
            Nu_Panel(lx - tw * 0.5f, ly - th * 0.5f, tw, th, on ? 0.95f : 0.75f);
            if (on) Nu_ShFrame(lx - tw * 0.5f, ly - th * 0.5f, tw, th, 1.0f, 0.95f);
            Nu_DrawTextCentered(t, lx, ly - Nu_TextH(px) * 0.5f, px,
                                on ? 1.0f : 0.80f, on ? 0.96f : 0.80f, on ? 0.70f : 0.86f, 1.0f);
        }
        Nu_DrawTextCentered("point and release, or press 1-8", cx, cy + rad + (float)px * 1.6f,
                            (int)((float)px * 0.8f), 0.62f, 0.72f, 1.0f, 0.9f);
    }

    /* Co-op: a scene waiting for a partner to load, or one we are joining. */
    {
        char line[128];
        if (ShNetCut_StatusText(line, (int)sizeof(line)))
        {
            const float w = s_vpW * 0.40f, h = (float)px * 2.4f;
            const float x = (s_vpW - w) * 0.5f, y = s_vpH * 0.30f;
            Nu_Panel(x, y, w, h, 0.88f);
            Nu_ShFrame(x, y, w, h, 1.0f, 0.9f);
            Nu_DrawTextCentered(line, s_vpW * 0.5f, y + (float)px * 0.7f, px, 0.92f, 0.90f, 0.80f, 1.0f);
        }
    }

    if (ShNetChat_DisplayActive())
    {
        Nu_DrawChat((int)((float)px * 0.82f));
    }

    if (!drawComposer && ShNet_CoopLivingWorld())
    {
        Nu_DrawNearbyMemo(px);
    }
    /* Co-op: my own health, bottom left, when asked for. */
    {
        extern int g_PcCoopGame;
        int hp = ShNet_CoopSelfHealthPct();
        if (g_PcCoopGame && (g_PcConfig.onlineHealthBars & 1) && hp >= 0 && !Pc_CoopMenu_IsOpen())
        {
            const float w  = s_vpW * 0.16f, h = (float)px * 0.55f;
            const float x  = s_vpH * 0.04f, y = s_vpH * 0.93f;
            float       f;
            f = (float)hp / 100.0f;
            Nu_FillPx(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 0.03f, 0.03f, 0.08f, 0.75f);
            Nu_FillPx(x, y, w * f, h,
                      hp > 50 ? 0.30f + (100 - hp) * 0.012f : 0.86f,
                      hp > 50 ? 0.78f : 0.24f + hp * 0.011f, 0.20f, 0.95f);
            Nu_ShFrame(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 1.0f, 0.85f);
        }
    }
    if (drawList)
    {
        Nu_DrawPlayerList(px);
    }
    if (drawComposer)
    {
        Nu_DrawComposer(px);
    }
    /* The co-op menu is drawn by pc_coop_screen.c with the game's own
     * primitives now; the GL drawer below is kept for reference. */
    (void)Nu_DrawCoopMenu;

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glActiveTexture((GLenum)prevActive);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb,
                        (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend)   glDisable(GL_BLEND);
    if (prevDepth)    glEnable(GL_DEPTH_TEST);
    if (prevCull)     glEnable(GL_CULL_FACE);
    if (prevScissor)  glEnable(GL_SCISSOR_TEST);
}
