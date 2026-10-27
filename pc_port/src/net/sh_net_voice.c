/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_voice.c - the vocalize wheel (co-op callouts).
 *
 * Hold the vocalize key: a wheel of eight short lines for the character you
 * play appears; point at one with the mouse (or press its number) and let go.
 * The line plays for you and for everyone on your map, from where you stand.
 *
 * The lines are the game's own voice clips, decoded from the disc on first
 * use (XaPlayer_DecodeLine) and kept; or, when a voice pack is set, WAVs from
 * gamedata/voices/<pack>/<character>/<slot>.wav. Only (character, slot, xa
 * index) travels: each machine resolves the sound from its own files, so no
 * one can make another machine load anything it does not already have.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"

#include "pc_config.h"
#include "pc_mouse_cursor.h"
#include "pc_playas.h"
#include "pc_voice_clip.h"
#include "xa_player.h"
#include "xa_wav.h"
#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_session.h"
#include "sh_net_platform.h"
#include "sh_net_voice.h"
#include "sh_log.h"

#define VOICE_SLOTS   8
#define VOICE_CHARAS  5
#define VOICE_MIN_GAP 1500u   /* per speaker */
#define VOICE_HEAR_U  30      /* units: full volume to silence over this range */

typedef struct { unsigned short xa; const char* label; } VoiceLine;
typedef struct { int charaId; const char* folder; VoiceLine line[VOICE_SLOTS]; } VoiceSet;

/* Disc lines, short and standalone, by character (xa indices into
 * g_XaItemData). The speaker is not the index's neighbourhood: each disc file
 * sorts its clips by length, so one character's lines are interleaved with
 * everyone's. Each line here was checked against its scene's dialogue order
 * and the clip's voice pitch. */
static const VoiceSet s_sets[VOICE_CHARAS] = {
    { Chara_Harry, "harry", {
        { 44,  "Harry Mason." },      { 28,  "Yeah. Thanks." },    { 243, "Okay." },        { 545, "Where's Cheryl?" },
        { 464, "Wait!" },             { 561, "Damn!" },            { 42,  "What's that?" },   { 205, "Stop! Don't shoot!" } } },
    { Chara_Cybil, "cybil", {
        { 45,  "Cybil Bennett." },    { 40,  "Glad to hear it." }, { 49,  "Got it?" },      { 249, "Alright." },
        { 209, "I'll cover you." },   { 195, "Be careful." },      { 231, "Harry!" },       { 26,  "It's dangerous out there." } } },
    { Chara_Kaufmann, "kaufmann", {
        { 454, "Give me that!" },     { 463, "Got it?" },          { 430, "None of your business!" }, { 452, "What are you doing?" },
        { 705, "Nobody uses me!" },   { 706, "Did I ask for this!" }, { 704, "You're easy!" }, { 252, "Hold it..." } } },
    { Chara_Lisa, "lisa", {
        { 365, "Harry!" },            { 159, "Harry, let me help you." },  { 212, "It's awful..." }, { 547, "Harry, listen." },
        { 355, "Harry, don't go!" },  { 221, "I'm sorry." },       { 558, "No." },          { 508, "Harry... help me..." } } },
    { Chara_Dahlia, "dahlia", {
        { 234, "Darkness." },         { 552, "Alessa." },          { 542, "Bad girl." },    { 536, "I was careless." },
        { 76,  "I knew you'd come." },{ 183, "You were too late." },{ 423, "Before it is too late." }, { 517, "But what a pity, yes?" } } },
};

/* Decoded clips, kept once made: [set][slot]. */
typedef struct { short* pcm; unsigned int samples; int rate, stereo, tried; } VoiceClip;
static VoiceClip s_clips[VOICE_CHARAS][VOICE_SLOTS];

static int          s_open;        /* the wheel is up */
static int          s_sel = -1;    /* slot under the pointer, -1 none */
static int          s_heldPrev;
static unsigned int s_lastSentMs;
static struct { unsigned long long id; unsigned int ms; } s_lastHeard[SHSESSION_MAX_MEMBERS];

static int Voice_SetFor(int charaId)
{
    int i;
    for (i = 0; i < VOICE_CHARAS; i++)
    {
        if (s_sets[i].charaId == charaId) return i;
    }
    return 0; /* skins without a voice of their own speak as Harry */
}

/* The sound for (set, slot): the voice pack's WAV when there is one, else
 * the disc line. Decoded once. */
static const VoiceClip* Voice_Clip(int set, int slot)
{
    VoiceClip* c = &s_clips[set][slot];
    if (c->tried)
    {
        return c->pcm ? c : NULL;
    }
    c->tried = 1;
    if (g_PcConfig.voicePack[0])
    {
        extern const char* PcPort_GetGameDataPath(void);
        char           path[1024];
        unsigned char* pcm   = NULL;
        unsigned int   bytes = 0;
        int            rate = 0, stereo = 0;
        snprintf(path, sizeof(path), "%s/voices/%s/%s/%d.wav", PcPort_GetGameDataPath(),
                 g_PcConfig.voicePack, s_sets[set].folder, slot + 1);
        if (XaWav_Load(path, &pcm, &bytes, &rate, &stereo) && pcm && bytes >= 4)
        {
            c->pcm     = (short*)pcm;
            c->samples = bytes / 2u;
            c->rate    = rate;
            c->stereo  = stereo;
            SH_DBG("[VOICE] %s/%s/%d.wav loaded (%u samples, %d Hz)", g_PcConfig.voicePack, s_sets[set].folder, slot + 1, c->samples, rate);
            return c;
        }
    }
    {
        short*       pcm = NULL;
        unsigned int n   = 0;
        int          rate = 0, stereo = 0;
        if (XaPlayer_DecodeLine(s_sets[set].line[slot].xa, &pcm, &n, &rate, &stereo) && pcm)
        {
            c->pcm = pcm; c->samples = n; c->rate = rate; c->stereo = stereo;
            SH_DBG("[VOICE] disc line %u decoded (%u samples, %d Hz) for %s %d",
                   (unsigned)s_sets[set].line[slot].xa, n, rate, s_sets[set].folder, slot + 1);
            return c;
        }
    }
    SH_DBG("[VOICE] no sound for %s line %d", s_sets[set].folder, slot + 1);
    return NULL;
}

static void Voice_Play(int set, int slot, float gain)
{
    const VoiceClip* c = Voice_Clip(set, slot);
    if (c)
    {
        Pc_VoiceClip_Play(c->pcm, c->samples, c->rate, c->stereo, gain);
    }
}

static void Voice_Say(int slot)
{
    const unsigned int now = ShNetPlat_Millis();
    int                set = Voice_SetFor(Pc_PlayAs_SkinCharaId());
    unsigned char      b[6];
    int                o = 0;

    if (slot < 0 || slot >= VOICE_SLOTS || now - s_lastSentMs < VOICE_MIN_GAP)
    {
        return;
    }
    s_lastSentMs = now;
    Voice_Play(set, slot, 1.0f);
    if (ShSession_Active())
    {
        b[o++] = SHNET_CTRL_VOICE;
        b[o++] = (unsigned char)s_sets[set].charaId;
        b[o++] = (unsigned char)slot;
        ShnPutU16(b, &o, s_sets[set].line[slot].xa);
        ShSession_SendGame(SHNET_MSG_S_CTRL, b, o, 0, 0);
    }
}

void ShNetVoice_KeyHeld(int held)
{
    extern int g_PcCoopGame;
    const unsigned char* ks = SDL_GetKeyboardState(NULL);
    int inGame = g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
                 !(g_SysWork.sysFlags & SysFlag_DemoActive);

    if (!g_PcCoopGame || !inGame)
    {
        s_open = 0; s_sel = -1; s_heldPrev = 0;
        return;
    }
    if (held && !s_heldPrev)
    {
        s_open = 1;
        s_sel  = -1;
    }
    if (s_open)
    {
        float nx, ny;
        int   k;
        if (Pc_MouseCursor_ViewportPos(&nx, &ny))
        {
            float dx = nx - 0.5f, dy = ny - 0.5f;
            if (dx * dx + dy * dy > 0.0025f) /* past the dead centre */
            {
                /* Slot 0 at the top, clockwise. */
                float a = (float)atan2((double)dx, (double)-dy); /* 0 = up, +cw */
                if (a < 0.0f) a += 6.2831853f;
                s_sel = ((int)((a + 6.2831853f / 16.0f) / (6.2831853f / 8.0f))) % VOICE_SLOTS;
            }
        }
        if (ks)
        {
            for (k = 0; k < VOICE_SLOTS; k++)
            {
                if (ks[SDL_SCANCODE_1 + k]) s_sel = k;
            }
        }
    }
    if (!held && s_heldPrev && s_open)
    {
        s_open = 0;
        if (s_sel >= 0) Voice_Say(s_sel);
        s_sel = -1;
    }
    s_heldPrev = held;
}

void ShNetVoice_Tick(void)
{
    Pc_VoiceClip_Tick();
}

void ShNetVoice_CtrlIn(unsigned long long from, const unsigned char* p, int len)
{
    const unsigned int now = ShNetPlat_Millis();
    int                o = 1, chara, slot, set, i, n, k = -1;
    unsigned int       xa;
    float              gain = 0.0f;

    if (len < 5 || from == ShSession_SelfId())
    {
        return;
    }
    chara = p[o++];
    slot  = p[o++];
    xa    = ShnGetU16(p, &o);
    if (slot < 0 || slot >= VOICE_SLOTS)
    {
        return;
    }
    set = Voice_SetFor(chara);
    /* The xa index is theirs to say but ours to play: it must be the line
     * our table has for that slot (a pack of our own may replace it). */
    if (xa != s_sets[set].line[slot].xa)
    {
        return;
    }
    /* One line per speaker per second and a half, whatever they send. */
    for (i = 0; i < SHSESSION_MAX_MEMBERS; i++)
    {
        if (s_lastHeard[i].id == from) { k = i; break; }
        if (s_lastHeard[i].id == 0 && k < 0) k = i;
    }
    if (k < 0) k = 0;
    if (s_lastHeard[k].id == from && now - s_lastHeard[k].ms < VOICE_MIN_GAP)
    {
        return;
    }
    s_lastHeard[k].id = from;
    s_lastHeard[k].ms = now;

    /* Loudness by distance to where they stand; nothing from another map. */
    n = ShSession_MemberCount();
    for (i = 0; i < n; i++)
    {
        const ShSessionMember* m = ShSession_Member(i);
        ShSessionPoseNow       pn;
        if (!m || m->steamId != from) continue;
        if (m->mapIdx != (int)g_SavegamePtr->mapIdx || !ShSession_MemberPoseNow(i, &pn)) return;
        {
            long long dx = (long long)pn.x - g_SysWork.playerWork.player.position.vx;
            long long dz = (long long)pn.z - g_SysWork.playerWork.player.position.vz;
            double    d  = sqrt((double)(dx * dx + dz * dz)) / 4096.0;
            gain = (float)(1.0 - d / (double)VOICE_HEAR_U);
            if (gain < 0.08f) gain = (d < VOICE_HEAR_U * 2) ? 0.08f : 0.0f;
        }
        break;
    }
    if (gain <= 0.0f)
    {
        return;
    }
    Voice_Play(set, slot, gain);
}

int ShNetVoice_MenuOpen(void)
{
    return s_open;
}

int ShNetVoice_MenuSelected(void)
{
    return s_sel;
}

const char* ShNetVoice_MenuLabel(int slot)
{
    int set = Voice_SetFor(Pc_PlayAs_SkinCharaId());
    if (slot < 0 || slot >= VOICE_SLOTS) return "";
    return s_sets[set].line[slot].label;
}
