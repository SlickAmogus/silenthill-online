/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_voice_clip.c - one-shot PCM clips on their own OpenAL sources.
 *
 * The game's voice channel is one stream (xa_player.c): starting anything on
 * it cuts the cutscene line that is playing and tells the scene script the
 * line finished. Co-op callouts therefore get their own small set of sources
 * on the same OpenAL context PsyCross owns, fed with already decoded PCM and
 * forgotten when done.
 */

#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>

#include "pc_voice_clip.h"
#include "sh_log.h"

#define VC_SOURCES 4

typedef struct
{
    ALuint source;
    ALuint buffer;
    int    busy;
} VcSlot;

static VcSlot s_slots[VC_SOURCES];
static int    s_ready;

static int Vc_Ready(void)
{
    int i;
    if (s_ready)
    {
        return 1;
    }
    if (alcGetCurrentContext() == NULL)
    {
        return 0; /* PsyCross has not brought audio up (or runs without it) */
    }
    for (i = 0; i < VC_SOURCES; i++)
    {
        alGenSources(1, &s_slots[i].source);
        alGenBuffers(1, &s_slots[i].buffer);
        s_slots[i].busy = 0;
    }
    s_ready = (alGetError() == AL_NO_ERROR);
    return s_ready;
}

void Pc_VoiceClip_Tick(void)
{
    int i;
    if (!s_ready)
    {
        return;
    }
    for (i = 0; i < VC_SOURCES; i++)
    {
        ALint st = AL_STOPPED;
        if (!s_slots[i].busy) continue;
        alGetSourcei(s_slots[i].source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING && st != AL_PAUSED)
        {
            alSourcei(s_slots[i].source, AL_BUFFER, 0);
            s_slots[i].busy = 0;
        }
    }
}

int Pc_VoiceClip_Play(const short* pcm, unsigned int samples, int rate, int stereo, float gain)
{
    int i, pick = -1;
    if (!pcm || samples == 0 || rate <= 0 || !Vc_Ready())
    {
        return 0;
    }
    Pc_VoiceClip_Tick();
    for (i = 0; i < VC_SOURCES; i++)
    {
        if (!s_slots[i].busy) { pick = i; break; }
    }
    if (pick < 0)
    {
        /* All talking at once: the oldest gives way. */
        pick = 0;
        alSourceStop(s_slots[0].source);
        alSourcei(s_slots[0].source, AL_BUFFER, 0);
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    alBufferData(s_slots[pick].buffer, stereo ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16,
                 pcm, (ALsizei)(samples * sizeof(short)), rate);
    alSourcei(s_slots[pick].source, AL_BUFFER, (ALint)s_slots[pick].buffer);
    alSourcef(s_slots[pick].source, AL_GAIN, gain);
    alSourcePlay(s_slots[pick].source);
    s_slots[pick].busy = (alGetError() == AL_NO_ERROR);
    return s_slots[pick].busy;
}
