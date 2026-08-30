// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdkconfig.h"

#if CONFIG_TD_AUDIO_PDM

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "deh_str.h"
#include "doom/sounds.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define AUDIO_RATE 22050
#define MIX_FRAMES 256
#define ADPCM_BLOCK_SIZE 128
#define ADPCM_SAMPLES_PER_BLOCK 249
#define SOUND_CHANNELS 8

static const char *TAG = "doom_audio";
static i2s_chan_handle_t tx_channel;
static portMUX_TYPE mixer_lock = portMUX_INITIALIZER_UNLOCKED;
static int sound_initialized;
static int use_sfx_prefix;

typedef struct {
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t offset;
    uint32_t step;
    uint8_t left, right;
    uint8_t decoded_count;
    int filtered;
    int8_t decoded[ADPCM_SAMPLES_PER_BLOCK];
} sound_channel_t;

static sound_channel_t channels[SOUND_CHANNELS];
static int32_t mix_buffer[MIX_FRAMES];
static int16_t pcm_buffer[MIX_FRAMES];

static const uint16_t step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
    34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,
    598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
    1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
    5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,
    13899,15289,16818,18500,20350,22385,24623,27086,29794,32767,
};
static const int8_t index_table[8] = {-1,-1,-1,-1,2,4,6,8};

static int clip(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static int decode_adpcm(int8_t *out, const uint8_t *in, int length)
{
    if (length < 4 || in[2] > 88 || in[3]) return 0;
    int sample = (int16_t)(in[0] | in[1] << 8);
    int index = in[2];
    int count = 1;
    *out++ = sample >> 8;
    in += 4;
    length -= 4;
    while (length >= 4) {
        for (int byte = 0; byte < 4; ++byte) {
            uint8_t packed = *in++;
            for (int half = 0; half < 2; ++half) {
                int code = half ? packed >> 4 : packed & 15;
                int step = step_table[index];
                int delta = step >> 3;
                if (code & 1) delta += step >> 2;
                if (code & 2) delta += step >> 1;
                if (code & 4) delta += step;
                if (code & 8) delta = -delta;
                sample = clip(sample + delta, -32768, 32767);
                index = clip(index + index_table[code & 7], 0, 88);
                *out++ = sample >> 8;
                ++count;
            }
        }
        length -= 4;
    }
    return count;
}

static void decode_next(sound_channel_t *channel)
{
    if (channel->data >= channel->data_end) {
        channel->decoded_count = 0;
        return;
    }
    int bytes = channel->data_end - channel->data;
    if (bytes > ADPCM_BLOCK_SIZE) bytes = ADPCM_BLOCK_SIZE;
    int count = decode_adpcm(channel->decoded, channel->data, bytes);
    channel->data += bytes;
    channel->decoded_count = count;
}

static void get_lump_name(const sfxinfo_t *sfx, char *name, size_t size)
{
    if (sfx->link) sfx = sfx->link;
    if (use_sfx_prefix) M_snprintf(name, size, "ds%s", DEH_String(sfx->name));
    else M_StringCopy(name, DEH_String(sfx->name), size);
}

static void mix_audio(void)
{
    memset(mix_buffer, 0, sizeof(mix_buffer));
    portENTER_CRITICAL(&mixer_lock);
    for (unsigned c = 0; c < SOUND_CHANNELS; ++c) {
        sound_channel_t *channel = &channels[c];
        if (!channel->decoded_count) continue;
        int volume = (channel->left + channel->right) / 4;
        for (unsigned frame = 0; frame < MIX_FRAMES; ++frame) {
            uint32_t end = channel->decoded_count << 16;
            if (channel->offset >= end) {
                channel->offset -= end;
                decode_next(channel);
                if (!channel->decoded_count) break;
            }
            int raw = channel->decoded[channel->offset >> 16];
            channel->filtered = (channel->filtered * 3 + raw) / 4;
            mix_buffer[frame] += channel->filtered * volume;
            channel->offset += channel->step;
        }
    }
    portEXIT_CRITICAL(&mixer_lock);
    for (unsigned i = 0; i < MIX_FRAMES; ++i)
        pcm_buffer[i] = clip(mix_buffer[i], -32768, 32767);
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        mix_audio();
        size_t written = 0;
        esp_err_t result = i2s_channel_write(tx_channel, pcm_buffer,
                                             sizeof(pcm_buffer), &written,
                                             portMAX_DELAY);
        if (result != ESP_OK || written != sizeof(pcm_buffer))
            ESP_LOGW(TAG, "PDM write: %s (%u bytes)",
                     esp_err_to_name(result), (unsigned)written);
    }
}

isb_int8_t snd_pitchshift = 0;

void I_InitSound(boolean prefix)
{
    use_sfx_prefix = prefix;
    i2s_chan_config_t channel_config =
            I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = 128;
    channel_config.auto_clear = true;
    esp_err_t result = i2s_new_channel(&channel_config, &tx_channel, NULL);
    if (result != ESP_OK) goto fail;
    i2s_pdm_tx_config_t pdm = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = GPIO_NUM_43,
            .dout = GPIO_NUM_44,
            .invert_flags.clk_inv = false,
        },
    };
    if ((result = i2s_channel_init_pdm_tx_mode(tx_channel, &pdm)) != ESP_OK ||
        (result = i2s_channel_enable(tx_channel)) != ESP_OK)
        goto fail;
    sound_initialized = 1;
    if (xTaskCreatePinnedToCore(audio_task, "doom_audio", 2304, NULL, 5,
                                NULL, 0) != pdPASS) {
        sound_initialized = 0;
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "8-channel ADPCM mixer -> 22.05 kHz PDM on GPIO44");
    return;
fail:
    ESP_LOGE(TAG, "PDM audio disabled: %s", esp_err_to_name(result));
}

void I_ShutdownSound(void) { sound_initialized = 0; }

int I_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char name[9];
    get_lump_name(sfx, name, sizeof(name));
    return W_GetNumForName(name);
}

void I_UpdateSound(void) {}

void I_UpdateSoundParams(int handle, int volume, int separation)
{
    if (!sound_initialized || (unsigned)handle >= SOUND_CHANNELS) return;
    int left = clip(((254 - separation) * volume) / 127, 0, 255);
    int right = clip((separation * volume) / 127, 0, 255);
    portENTER_CRITICAL(&mixer_lock);
    channels[handle].left = left;
    channels[handle].right = right;
    portEXIT_CRITICAL(&mixer_lock);
}

int I_StartSound(should_be_const sfxinfo_t *sfx, int handle, int volume,
                 int separation, int pitch)
{
    if (!sound_initialized || (unsigned)handle >= SOUND_CHANNELS) return -1;
    int lump = sfx_mut(sfx)->lumpnum;
    int length = W_LumpLength(lump);
    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
    if (length < 8 || data[0] != 0x03 || data[1] != 0x80) return -1;
    uint32_t rate = data[2] | data[3] << 8;
    portENTER_CRITICAL(&mixer_lock);
    sound_channel_t *channel = &channels[handle];
    memset(channel, 0, sizeof(*channel));
    channel->data = data + 8;
    channel->data_end = data + length;
    channel->step = (uint32_t)((uint64_t)rate * pitch * 65536 /
                               (AUDIO_RATE * NORM_PITCH));
    channel->left = clip(((254 - separation) * volume) / 127, 0, 255);
    channel->right = clip((separation * volume) / 127, 0, 255);
    decode_next(channel);
    channel->filtered = channel->decoded_count ? channel->decoded[0] : 0;
    portEXIT_CRITICAL(&mixer_lock);
    return handle;
}

void I_StopSound(int handle)
{
    if ((unsigned)handle >= SOUND_CHANNELS) return;
    portENTER_CRITICAL(&mixer_lock);
    channels[handle].decoded_count = 0;
    portEXIT_CRITICAL(&mixer_lock);
}

boolean I_SoundIsPlaying(int handle)
{
    return sound_initialized && (unsigned)handle < SOUND_CHANNELS &&
           channels[handle].decoded_count != 0;
}

void I_PrecacheSounds(should_be_const sfxinfo_t *sounds, int count)
{ (void)sounds; (void)count; }

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(void) {}
void I_ResumeSong(void) {}
void *I_RegisterSong(should_be_const void *data, int len)
{ (void)data; (void)len; return NULL; }
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
