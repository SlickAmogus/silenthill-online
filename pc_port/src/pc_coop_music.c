/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_music.c - music for the multiplayer front end.
 *
 * The title screen is silent (the theme belongs to the opening movie), so
 * the multiplayer menu and a room being waited in play a BGM track, driven
 * the way a map drives its track (Bgm_Init's stop/load, then Bgm_Update
 * each frame with every layer up). The results-screen piece (track 5) by
 * default; the menu's Music row picks any of the game's tracks and keeps
 * the pick in config. Stopped the moment the menu closes or a game boots,
 * with the loaded-track index cleared so the map's own Bgm_Init loads its
 * track as usual.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bgm.h"
#include "bodyprog/sound/sound_system.h"
#include "main/fsqueue.h"
#include "pc_config.h"
#include "pc_coop_menu.h"
#include "sh_net_session.h"
#include "sh_log.h"

#define RESULTS_TRACK BgmTrackIdx_5
#define TRACK_FIRST   BgmTrackIdx_2  /* 0 is none and 1 the alley's special case: neither loads */
#define TRACK_LAST    BgmTrackIdx_41

static int Music_Track(void)
{
    int t = g_PcConfig.onlineMenuBgm;
    return (t < TRACK_FIRST || t > TRACK_LAST) ? RESULTS_TRACK : t;
}

enum { LM_OFF = 0, LM_STOPPING, LM_PLAYING };

static int s_step;

static int Music_Wanted(void)
{
    return g_GameWork.gameState == GameState_MainMenu &&
           (Pc_CoopMenu_IsOpen() || ShSession_Active());
}

static void Music_Stop(const char* why)
{
    SD_Call(18); /* Sd_StopBgmTaskAdd */
    Bgm_AllLayersMute();
    g_GameWork.bgmIdx = BgmTrackIdx_None;
    s_step = LM_OFF;
    SH_DBG("[COOP] lobby music off: %s", why);
}

void Pc_CoopMusic_Tick(void)
{
    if (!Music_Wanted())
    {
        if (s_step != LM_OFF)
        {
            Music_Stop(g_GameWork.gameState == GameState_MainMenu ? "menu closed" : "leaving the title");
        }
        return;
    }
    switch (s_step)
    {
        case LM_OFF:
            /* Whatever was left playing (a game just left) goes first. */
            SD_Call(18);
            Bgm_AllLayersMute();
            s_step = LM_STOPPING;
            break;

        case LM_STOPPING:
            if (Sd_AudioStreamingCheck() || Fs_QueueGetLength() > 0 || func_80045BC8() != 0)
            {
                break;
            }
            Bgm_TrackSet(Music_Track());
            s_step = LM_PLAYING;
            SH_DBG("[COOP] lobby music: BGM track %d", Music_Track());
            break;

        case LM_PLAYING:
            /* Bgm_Update reads a health of 0 as "dead: fade to silence", and
             * the title has no player. Never put the old value back: a New
             * Game boots inside the title's own update, before this tick,
             * and restoring 0 after it started every player dead in the
             * alley. The boot and a loaded save set the real value. */
            if (g_SysWork.playerWork.player.health <= Q12(0.0f))
            {
                g_SysWork.playerWork.player.health = Q12(100.0f);
            }
            /* Every layer up, at the results screen's own fade rate. The
             * KeepAlive bit keeps the radio out of it. */
            Bgm_Update(BgmFlag_Layer1 | BgmFlag_Layer2 | BgmFlag_Layer3 | BgmFlag_Layer4 |
                       BgmFlag_Layer5 | BgmFlag_Layer6 | BgmFlag_Layer7 | BgmFlag_KeepAlive,
                       Q12(240.0f), NULL);
            break;
    }
}

/* Where each track is heard, for the Music row. Taken from the map headers
 * and the maps' own track changes; a track no map names is just its number. */
static const char* const s_trackPlace[TRACK_LAST + 1] = {
    NULL, NULL,
    "Hospital (Otherworld)",            /* 2 */
    "Streets",                          /* 3 */
    "Nowhere",                          /* 4 */
    "Results",                          /* 5 */
    "Old Silent Hill",                  /* 6 */
    "Resort Area",                      /* 7 */
    "School",                           /* 8 */
    "Cafe",                             /* 9 */
    "Nowhere",                          /* 10 */
    "School (Otherworld)",              /* 11 */
    "Sewers",                           /* 12 */
    "Central Silent Hill (Otherworld)", /* 13 */
    "Nowhere",                          /* 14 */
    "Nowhere",                          /* 15 */
    "Hospital",                         /* 16 */
    "Central Silent Hill (Otherworld)", /* 17 */
    "School",                           /* 18 */
    "Hospital (Otherworld)",            /* 19 */
    "Resort Area (Otherworld)",         /* 20 */
    "Lighthouse",                       /* 21 */
    "Nowhere",                          /* 22 */
    NULL,                               /* 23 */
    "Antiques Shop",                    /* 24 */
    NULL,                               /* 25 */
    "School",                           /* 26 */
    "Church",                           /* 27 */
    "Sewers",                           /* 28 */
    "Nowhere",                          /* 29 */
    "Cafe",                             /* 30 */
    NULL,                               /* 31 */
    "Hospital",                         /* 32 */
    "Shopping Center",                  /* 33 */
    "Motel",                            /* 34 */
    "Nowhere",                          /* 35 */
    "Amusement Park",                   /* 36 */
    "Amusement Park",                   /* 37 */
    "Central Silent Hill",              /* 38 */
    NULL,                               /* 39 */
    "Nowhere",                          /* 40 */
    "Lighthouse",                       /* 41 */
};

void Pc_CoopMusic_RowText(char* out, int cap)
{
    const int t = Music_Track();
    if (t == RESULTS_TRACK)
    {
        snprintf(out, cap, "Music:  Results");
    }
    else if (s_trackPlace[t] != NULL)
    {
        snprintf(out, cap, "Music:  Track %d (%s)", t, s_trackPlace[t]);
    }
    else
    {
        snprintf(out, cap, "Music:  Track %d", t);
    }
}

int Pc_CoopMusic_IsDefault(void)
{
    return Music_Track() == RESULTS_TRACK;
}

/* The pick is config, and a track already playing gives way to the new one
 * on the next tick: LM_OFF stops it and LM_STOPPING loads the new one. */
static void Music_Set(int track)
{
    char v[8];
    g_PcConfig.onlineMenuBgm = track;
    snprintf(v, sizeof(v), "%d", track);
    PcConfig_SaveKeyValue("online_menu_music", v);
    if (s_step == LM_PLAYING)
    {
        s_step = LM_OFF;
    }
    SH_DBG("[COOP] lobby music picked: BGM track %d", track);
}

void Pc_CoopMusic_Step(int dir)
{
    int t = Music_Track() + dir;
    if (t > TRACK_LAST)  t = TRACK_FIRST;
    if (t < TRACK_FIRST) t = TRACK_LAST;
    Music_Set(t);
}

void Pc_CoopMusic_Reset(void)
{
    Music_Set(RESULTS_TRACK);
}
