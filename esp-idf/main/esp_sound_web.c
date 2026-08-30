// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdkconfig.h"

#if CONFIG_TD_WEB_AUDIO && !CONFIG_TD_AUDIO_PDM

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"

#include "config.h"
#include "deh_str.h"
#include "doom/sounds.h"
#include "doomtype.h"
#include "esp_web_audio.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define WEB_SOUND_CHANNELS 8
#define AUDIO_MAGIC 0xDA

enum {
    AUDIO_RESET = 0,
    AUDIO_SFX_START = 1,
    AUDIO_SFX_STOP = 2,
    AUDIO_SFX_PARAMS = 3,
    AUDIO_MUSIC_PLAY = 4,
    AUDIO_MUSIC_STOP = 5,
    AUDIO_MUSIC_PAUSE = 6,
    AUDIO_MUSIC_RESUME = 7,
    AUDIO_MUSIC_VOLUME = 8,
};

typedef struct {
    int64_t end_us;
    uint8_t active;
    uint8_t sfx_id;
    uint8_t volume;
    uint8_t separation;
    uint8_t pitch;
} web_channel_t;

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static web_channel_t channels[WEB_SOUND_CHANNELS];
static int use_sfx_prefix;
static uint8_t music_id;
static uint8_t music_volume = 64;
static uint8_t music_looping;
static uint8_t music_playing;
static uint8_t music_paused;
static int64_t music_started_us;
static int64_t music_pause_started_us;
static int64_t music_paused_total_us;

isb_int8_t snd_pitchshift = 0;

void I_StopSong(void);

static uint8_t clamp_byte(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static void send_reset(void)
{
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_RESET};
    esp_web_audio_publish(message, sizeof(message));
}

static void send_sfx_start(unsigned channel, const web_channel_t *state)
{
    const uint8_t message[] = {
        AUDIO_MAGIC, AUDIO_SFX_START, (uint8_t)channel, state->sfx_id,
        state->volume, state->separation, state->pitch,
    };
    esp_web_audio_publish(message, sizeof(message));
}

static void send_sfx_stop(unsigned channel)
{
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_SFX_STOP, (uint8_t)channel};
    esp_web_audio_publish(message, sizeof(message));
}

static void send_sfx_params(unsigned channel, uint8_t volume, uint8_t separation)
{
    const uint8_t message[] = {
        AUDIO_MAGIC, AUDIO_SFX_PARAMS, (uint8_t)channel, volume, separation,
    };
    esp_web_audio_publish(message, sizeof(message));
}

static void send_music_play(void)
{
    uint8_t id, looping, volume, paused;
    int64_t started_us, pause_started_us, paused_total_us;
    portENTER_CRITICAL(&state_lock);
    id = music_id;
    looping = music_looping;
    volume = music_volume;
    paused = music_paused;
    started_us = music_started_us;
    pause_started_us = music_pause_started_us;
    paused_total_us = music_paused_total_us;
    portEXIT_CRITICAL(&state_lock);
    int64_t at_us = paused ? pause_started_us : esp_timer_get_time();
    uint32_t elapsed_ms = started_us
            ? (uint32_t)((at_us - started_us - paused_total_us) / 1000)
            : 0;
    const uint8_t message[] = {
        AUDIO_MAGIC, AUDIO_MUSIC_PLAY, id, looping, volume,
        (uint8_t)elapsed_ms, (uint8_t)(elapsed_ms >> 8),
        (uint8_t)(elapsed_ms >> 16), (uint8_t)(elapsed_ms >> 24),
    };
    esp_web_audio_publish(message, sizeof(message));
}

static int decoded_sample_count(const uint8_t *data, int length)
{
    int samples = 0;
    for (int offset = 8; offset < length; offset += 128) {
        int bytes = length - offset;
        if (bytes > 128) bytes = 128;
        if (bytes >= 4)
            samples += 1 + ((bytes - 4) / 4) * 8;
    }
    return samples;
}

static void get_lump_name(const sfxinfo_t *sfx, char *name, size_t size)
{
    if (sfx->link) sfx = sfx->link;
    if (use_sfx_prefix) M_snprintf(name, size, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(name, DEH_String(sfx->name), size);
}

void I_InitSound(boolean prefix)
{
    use_sfx_prefix = prefix;
    memset(channels, 0, sizeof(channels));
}

void I_ShutdownSound(void)
{
    portENTER_CRITICAL(&state_lock);
    memset(channels, 0, sizeof(channels));
    portEXIT_CRITICAL(&state_lock);
    send_reset();
}

int I_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char name[9];
    get_lump_name(sfx, name, sizeof(name));
    return W_GetNumForName(name);
}

void I_UpdateSound(void) {}

void I_UpdateSoundParams(int handle, int volume, int separation)
{
    if ((unsigned)handle >= WEB_SOUND_CHANNELS) return;
    uint8_t vol = clamp_byte(volume);
    uint8_t sep = clamp_byte(separation);
    int changed = 0;
    portENTER_CRITICAL(&state_lock);
    if (channels[handle].active &&
        (channels[handle].volume != vol || channels[handle].separation != sep)) {
        channels[handle].volume = vol;
        channels[handle].separation = sep;
        changed = 1;
    }
    portEXIT_CRITICAL(&state_lock);
    if (changed) send_sfx_params((unsigned)handle, vol, sep);
}

int I_StartSound(should_be_const sfxinfo_t *sfx, int handle, int volume,
                 int separation, int pitch)
{
    if ((unsigned)handle >= WEB_SOUND_CHANNELS) return -1;
    int lump = sfx_mut(sfx)->lumpnum;
    int length = W_LumpLength(lump);
    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
    if (length < 8 || data[0] != 0x03 ||
        (data[1] != 0x80 && data[1] != 0x00)) return -1;
    uint32_t rate = data[2] | data[3] << 8;
    int samples;
    if (data[1] == 0x80) {
        samples = decoded_sample_count(data, length);
    } else {
        uint32_t declared = data[4] | data[5] << 8 | data[6] << 16 |
                            (uint32_t)data[7] << 24;
        samples = declared > 32 ? (int)declared - 32 : 0;
    }
    web_channel_t state = {
        .end_us = esp_timer_get_time() +
                  (int64_t)samples * 1000000 * NORM_PITCH /
                  ((rate ? rate : 11025) * (pitch ? pitch : NORM_PITCH)),
        .active = 1,
        .sfx_id = (uint8_t)(sfx - S_sfx),
        .volume = clamp_byte(volume),
        .separation = clamp_byte(separation),
        .pitch = clamp_byte(pitch),
    };
    portENTER_CRITICAL(&state_lock);
    channels[handle] = state;
    portEXIT_CRITICAL(&state_lock);
    send_sfx_start((unsigned)handle, &state);
    return handle;
}

void I_StopSound(int handle)
{
    if ((unsigned)handle >= WEB_SOUND_CHANNELS) return;
    int was_active;
    portENTER_CRITICAL(&state_lock);
    was_active = channels[handle].active;
    channels[handle].active = 0;
    portEXIT_CRITICAL(&state_lock);
    if (was_active) send_sfx_stop((unsigned)handle);
}

boolean I_SoundIsPlaying(int handle)
{
    if ((unsigned)handle >= WEB_SOUND_CHANNELS) return false;
    int active;
    int64_t end_us;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    active = channels[handle].active;
    end_us = channels[handle].end_us;
    if (active && now >= end_us)
        channels[handle].active = active = 0;
    portEXIT_CRITICAL(&state_lock);
    return active;
}

void I_PrecacheSounds(should_be_const sfxinfo_t *sounds, int count)
{ (void)sounds; (void)count; }

void I_InitMusic(void) {}
void I_ShutdownMusic(void) { I_StopSong(); }

void I_SetMusicVolume(int volume)
{
    uint8_t value = clamp_byte(volume);
    portENTER_CRITICAL(&state_lock);
    music_volume = value;
    portEXIT_CRITICAL(&state_lock);
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_MUSIC_VOLUME, value};
    esp_web_audio_publish(message, sizeof(message));
}

void I_PauseSong(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (!music_paused) music_pause_started_us = now;
    music_paused = 1;
    portEXIT_CRITICAL(&state_lock);
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_MUSIC_PAUSE};
    esp_web_audio_publish(message, sizeof(message));
}

void I_ResumeSong(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    if (music_paused) music_paused_total_us += now - music_pause_started_us;
    music_paused = 0;
    portEXIT_CRITICAL(&state_lock);
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_MUSIC_RESUME};
    esp_web_audio_publish(message, sizeof(message));
}

void *I_RegisterSong(should_be_const void *data, int len)
{
    (void)len;
    for (unsigned i = 1; i < NUMMUSIC; ++i) {
        if (S_music[i].lumpnum > 0 &&
            W_CacheLumpNum(S_music[i].lumpnum, PU_STATIC) == data)
            return (void *)(uintptr_t)(i + 1);
    }
    return NULL;
}

void I_UnRegisterSong(void *handle) { (void)handle; }

void I_PlaySong(void *handle, boolean looping)
{
    uintptr_t encoded = (uintptr_t)handle;
    if (!encoded || encoded > NUMMUSIC) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&state_lock);
    music_id = (uint8_t)(encoded - 1);
    music_looping = !!looping;
    music_playing = 1;
    music_paused = 0;
    music_started_us = now;
    music_pause_started_us = 0;
    music_paused_total_us = 0;
    portEXIT_CRITICAL(&state_lock);
    send_music_play();
}

void I_StopSong(void)
{
    int was_playing;
    portENTER_CRITICAL(&state_lock);
    was_playing = music_playing;
    music_playing = 0;
    music_paused = 0;
    portEXIT_CRITICAL(&state_lock);
    if (!was_playing) return;
    const uint8_t message[] = {AUDIO_MAGIC, AUDIO_MUSIC_STOP};
    esp_web_audio_publish(message, sizeof(message));
}

boolean I_MusicIsPlaying(void)
{
    portENTER_CRITICAL(&state_lock);
    int playing = music_playing;
    portEXIT_CRITICAL(&state_lock);
    return playing;
}

void esp_web_audio_sync_state(void)
{
    web_channel_t snapshot[WEB_SOUND_CHANNELS];
    uint8_t playing, paused;
    portENTER_CRITICAL(&state_lock);
    memcpy(snapshot, channels, sizeof(snapshot));
    playing = music_playing;
    paused = music_paused;
    portEXIT_CRITICAL(&state_lock);

    send_reset();
    int64_t now = esp_timer_get_time();
    for (unsigned i = 0; i < WEB_SOUND_CHANNELS; ++i)
        if (snapshot[i].active && snapshot[i].end_us > now)
            send_sfx_start(i, &snapshot[i]);
    if (playing) {
        send_music_play();
        if (paused) {
            const uint8_t message[] = {AUDIO_MAGIC, AUDIO_MUSIC_PAUSE};
            esp_web_audio_publish(message, sizeof(message));
        }
    }
}

void I_BindSoundVariables(void) {}
void I_SetOPLDriverVer(opl_driver_ver_t version) { (void)version; }
void SafeUpdateSound(void) {}
boolean I_PicoSoundFading(void) { return false; }
uint8_t restart_song_state;

#endif
