// SPDX-License-Identifier: GPL-2.0-or-later
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "doomtype.h"
#include "i_sound.h"

#if !CONFIG_TD_AUDIO_PDM && !CONFIG_TD_WEB_AUDIO

isb_int8_t snd_pitchshift = 0;

void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) {}
int I_GetSfxLumpNum(should_be_const sfxinfo_t *sfx) { (void)sfx; return 0; }
void I_UpdateSound(void) {}
void I_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
int I_StartSound(should_be_const sfxinfo_t *sfx, int channel, int vol, int sep, int pitch) {
    (void)sfx; (void)channel; (void)vol; (void)sep; (void)pitch; return 0;
}
void I_StopSound(int channel) { (void)channel; }
boolean I_SoundIsPlaying(int channel) { (void)channel; return false; }
void I_PrecacheSounds(should_be_const sfxinfo_t *sounds, int count) { (void)sounds; (void)count; }
void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(should_be_const void *data, int len) { (void)data; (void)len; return NULL; }
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
void I_StopSong(void) {}
boolean I_MusicIsPlaying(void) { return false; }
void I_BindSoundVariables(void) {}
void I_SetOPLDriverVer(opl_driver_ver_t version) { (void)version; }
void SafeUpdateSound(void) {}
boolean I_PicoSoundFading(void) { return false; }
uint8_t restart_song_state;

#endif
