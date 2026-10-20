/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_VOICE_CLIP_H
#define PC_VOICE_CLIP_H

/* One-shot PCM clips on their own OpenAL sources (pc_voice_clip.c), apart
 * from the game's single voice stream. */

#ifdef __cplusplus
extern "C" {
#endif

/* Play interleaved 16-bit PCM once. The data is copied; the caller may free
 * it afterwards. gain 0..1. Returns 1 when it started. */
int  Pc_VoiceClip_Play(const short* pcm, unsigned int samples, int rate, int stereo, float gain);
/* Once a frame: recycle finished sources. */
void Pc_VoiceClip_Tick(void);

#ifdef __cplusplus
}
#endif

#endif
