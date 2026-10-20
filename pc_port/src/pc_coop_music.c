/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_coop_music.c - music for the multiplayer front end.
 *
 * The title screen is silent (the theme belongs to the opening movie), so
 * the multiplayer menu and a room being waited in play the piece the game
 * plays over the results screen after the credits: BGM track 5, driven the
 * way a map drives its track (Bgm_Init's stop/load, then Bgm_Update each
 * frame with every layer up). Stopped the moment the menu closes or a game
 * boots, with the loaded-track index cleared so the map's own Bgm_Init
 * loads its track as usual.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bgm.h"
#include "bodyprog/sound/sound_system.h"
#include "main/fsqueue.h"
#include "pc_coop_menu.h"
#include "sh_net_session.h"
#include "sh_log.h"

#define LOBBY_TRACK BgmTrackIdx_5

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
            Bgm_TrackSet(LOBBY_TRACK);
            s_step = LM_PLAYING;
            SH_DBG("[COOP] lobby music: BGM track %d", (int)LOBBY_TRACK);
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
