// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doomtype.h"
#include "esp_video_settings.h"
#include "doom/doomstat.h"
#include "doom/m_menu.h"
#include "i_input.h"
#include "i_video.h"
#include "pico/sem.h"
#include "picodoom.h"
#include "w_wad.h"
#include "z_zone.h"

/* Official T-Dongle-S3 schematic / LILYGO example pinout. */
enum {
    LCD_MOSI = 3, LCD_SCLK = 5, LCD_CS = 4, LCD_DC = 2,
    LCD_RST = 1, LCD_BL = 38,
    LCD_WIDTH = 160, LCD_HEIGHT = 80,
    LCD_STATUS_HEIGHT = 16,
    LCD_WORLD_HEIGHT = LCD_HEIGHT - LCD_STATUS_HEIGHT,
    LCD_X_GAP = 1, LCD_Y_GAP = 26,
};

#define LCD_HOST SPI2_HOST
#define LCD_DMA_ROWS 8
#define LCD_SPI_HZ (40 * 1000 * 1000)

boolean screensaver_mode;
isb_int8_t usegamma;
unsigned int joywait;
pixel_t *I_VideoBuffer;
/*
 * PicoDoom intentionally overlays a hidden 32-row status region around two
 * compact viewport pages.  Preserve its 80-row page stride, but provide real
 * guard banks at both ends so legal -32/+32 row scratch addressing can never
 * touch FreeRTOS objects or game globals on the ESP32-S3 linker layout.
 */
#define FRAME_PREFIX_ROWS 32
// A legacy full-screen patch can address a complete 200-row source relative
// to page 1 during transitions.  128 trailing rows cover that worst case.
#define FRAME_SUFFIX_ROWS 128
#define FRAME_STORAGE_BYTES (SCREENWIDTH * \
        (FRAME_PREFIX_ROWS + 2 * MAIN_VIEWHEIGHT + FRAME_SUFFIX_ROWS))
static uint8_t *frame_storage;
static uint8_t *transition_frame;
uint8_t (*frame_buffer)[SCREENWIDTH * MAIN_VIEWHEIGHT];
uint8_t (*status_buffer)[320 * 32];
semaphore_t render_frame_ready;
semaphore_t display_frame_freed;

uint8_t *wipe_yoffsets;
int16_t *wipe_yoffsets_raw;
uint32_t *wipe_linelookup;
uint8_t next_video_type;
uint8_t next_frame_index;
uint8_t next_overlay_index;
uint8_t *next_video_scroll;
volatile uint8_t wipe_min;

int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int fullscreen;
int aspect_ratio_correct;
int integer_scaling;
int vga_porch_flash;
int force_software_renderer;
should_be_const constcharstar video_driver = "tdongle-st7735-dma";
should_be_const constcharstar window_position = "";
boolean screenvisible = true;

static spi_device_handle_t lcd_spi;
static uint16_t palette565[256];
static DMA_ATTR uint16_t dma_rows[LCD_DMA_ROWS][LCD_WIDTH];
static const uint8_t *playpal;
static int playpal_len;
static unsigned lcd_brightness = 100;
static atomic_bool lcd_rotated;

void esp_video_set_brightness(unsigned percent)
{
    if (percent > 100) percent = 100;
    lcd_brightness = percent;
    /* The board's backlight switch is active-low. */
    uint32_t duty = (100 - percent) * ((1u << 13) - 1) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

unsigned esp_video_get_brightness(void)
{
    return lcd_brightness;
}

void esp_video_set_rotated(int rotated)
{
    atomic_store_explicit(&lcd_rotated, rotated != 0, memory_order_release);
}

int esp_video_get_rotated(void)
{
    return atomic_load_explicit(&lcd_rotated, memory_order_acquire);
}

static void lcd_tx(const void *data, size_t len, int dc)
{
    if (!len) return;
    gpio_set_level(LCD_DC, dc);
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

static void lcd_cmd(uint8_t command, const uint8_t *data, size_t len)
{
    lcd_tx(&command, 1, 0);
    lcd_tx(data, len, 1);
}

static void lcd_set_orientation(int rotated)
{
    // ST7735 MADCTL: landscape uses MV plus one mirror axis. Swapping MX for
    // MY rotates the panel exactly 180 degrees while preserving BGR order.
    uint8_t madctl = 0x20 | 0x08 | (rotated ? 0x80 : 0x40);
    lcd_cmd(0x36, &madctl, 1);
}

static void lcd_init(void)
{
    gpio_config_t output = {
        .pin_bit_mask = (1ULL << LCD_DC) | (1ULL << LCD_RST) | (1ULL << LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&output));
    gpio_set_level(LCD_BL, 1); /* active low: keep dark during init */
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI, .miso_io_num = -1, .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(dma_rows),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device = {
        .clock_speed_hz = LCD_SPI_HZ, .mode = 0,
        .spics_io_num = LCD_CS, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &device, &lcd_spi));

    static const uint8_t fr1[] = {0x05, 0x3a, 0x3a};
    static const uint8_t fr3[] = {0x05, 0x3a, 0x3a, 0x05, 0x3a, 0x3a};
    static const uint8_t pw1[] = {0x62, 0x02, 0x04};
    static const uint8_t pw3[] = {0x0d, 0x00};
    static const uint8_t pw4[] = {0x8d, 0x6a};
    static const uint8_t pw5[] = {0x8d, 0xee};
    static const uint8_t gp[] = {0x10,0x0e,0x02,0x03,0x0e,0x07,0x02,0x07,0x0a,0x12,0x27,0x37,0x00,0x0d,0x0e,0x10};
    static const uint8_t gn[] = {0x10,0x0e,0x03,0x03,0x0f,0x06,0x02,0x08,0x0a,0x13,0x26,0x36,0x00,0x0d,0x0e,0x10};
    uint8_t one;

    lcd_cmd(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(150)); /* SWRESET */
    lcd_cmd(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120)); /* SLPOUT */
    lcd_cmd(0xb1, fr1, sizeof(fr1)); lcd_cmd(0xb2, fr1, sizeof(fr1));
    lcd_cmd(0xb3, fr3, sizeof(fr3)); one = 0x03; lcd_cmd(0xb4, &one, 1);
    lcd_cmd(0xc0, pw1, sizeof(pw1)); one = 0xc0; lcd_cmd(0xc1, &one, 1);
    lcd_cmd(0xc2, pw3, sizeof(pw3)); lcd_cmd(0xc3, pw4, sizeof(pw4));
    lcd_cmd(0xc4, pw5, sizeof(pw5)); one = 0x0e; lcd_cmd(0xc5, &one, 1);
    one = 0x05; lcd_cmd(0x3a, &one, 1); /* RGB565 */
    lcd_set_orientation(esp_video_get_rotated());
    lcd_cmd(0xe0, gp, sizeof(gp)); lcd_cmd(0xe1, gn, sizeof(gn));
    lcd_cmd(0x21, NULL, 0); lcd_cmd(0x13, NULL, 0); lcd_cmd(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_channel = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&bl_channel));
    esp_video_set_brightness(lcd_brightness);
}

static inline uint16_t rgb565_wire(int r, int g, int b)
{
    uint16_t c = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    return (uint16_t)((c << 8) | (c >> 8));
}

static void build_palette(int palnum)
{
    if (!playpal) {
        lumpindex_t lump = W_GetNumForName("PLAYPAL");
        playpal = W_CacheLumpNum(lump, PU_STATIC);
        playpal_len = W_LumpLength(lump);
    }
    const uint8_t *source = playpal;
    int tint = 0, tr = 0, tg = 0, tb = 0;
    if (playpal_len >= (palnum + 1) * 768) source += palnum * 768;
    else if (palnum) {
        if (palnum < 9) { tint = palnum * 65536 / 9; tr = 255; }
        else if (palnum < 13) { tint = (palnum - 8) * 65536 / 8; tr = 215; tg = 186; tb = 69; }
        else { tint = 65536 / 8; tg = 256; }
    }
    for (int i = 0; i < 256; ++i) {
        int r = source[i * 3], g = source[i * 3 + 1], b = source[i * 3 + 2];
        if (tint) {
            r += ((tr - r) * tint) >> 16;
            g += ((tg - g) * tint) >> 16;
            b += ((tb - b) * tint) >> 16;
        }
        palette565[i] = rgb565_wire(r, g, b);
    }
}

static void lcd_begin_frame(void)
{
    uint16_t x0 = LCD_X_GAP, x1 = LCD_X_GAP + LCD_WIDTH - 1;
    uint16_t y0 = LCD_Y_GAP, y1 = LCD_Y_GAP + LCD_HEIGHT - 1;
    uint8_t col[] = {x0 >> 8, x0, x1 >> 8, x1};
    uint8_t row[] = {y0 >> 8, y0, y1 >> 8, y1};
    lcd_cmd(0x2a, col, sizeof(col)); lcd_cmd(0x2b, row, sizeof(row));
    uint8_t ramwr = 0x2c; lcd_tx(&ramwr, 1, 0);
}

static void show_poweron_pattern(void)
{
    static const uint16_t colors[] = {0x00f8, 0xe007, 0x1f00, 0xffff};
    uint16_t *out = &dma_rows[0][0];
    lcd_begin_frame();
    for (int y = 0; y < LCD_HEIGHT; y += LCD_DMA_ROWS) {
        for (int i = 0; i < LCD_DMA_ROWS * LCD_WIDTH; ++i)
            out[i] = colors[(i % LCD_WIDTH) / 40];
        lcd_tx(dma_rows, sizeof(dma_rows), 1);
    }
}

static void display_task(void *unused)
{
    (void)unused;
    uint32_t frames = 0;
    int last_gametic = gametic;
    uint16_t *out = &dma_rows[0][0];
    int64_t report_at = esp_timer_get_time() + 5000000;
    int previous_state = -1;
    int transition_frames = 0;
    int applied_rotation = -1;
    for (;;) {
        sem_acquire_blocking(&render_frame_ready);
        uint8_t frame = next_frame_index;
        int state = gamestate;
        if (previous_state == GS_DEMOSCREEN && state == GS_LEVEL) {
            // The first level frame toggles pages, leaving TITLEPIC intact in
            // the other page. Preserve it in the guarded suffix and perform a
            // compact Doom-style column melt without delaying game tics.
            memcpy(transition_frame, frame_buffer[frame ^ 1],
                   LCD_WIDTH * LCD_HEIGHT);
            transition_frames = 20;
        }
        previous_state = state;
        int requested_rotation = esp_video_get_rotated();
        if (requested_rotation != applied_rotation) {
            // Only this task owns LCD transactions after initialization, so
            // the orientation command can never split a frame transfer.
            lcd_set_orientation(requested_rotation);
            applied_rotation = requested_rotation;
        }
        lcd_begin_frame();
        for (int y = 0; y < LCD_HEIGHT; y += LCD_DMA_ROWS) {
            for (int i = 0; i < LCD_DMA_ROWS * LCD_WIDTH; ++i) {
                int screen_x = i % LCD_WIDTH;
                int screen_y = y + i / LCD_WIDTH;
                uint8_t pixel;
                if (M_MenuWantsCompactScale()) {
                    // The menu compositor has already mapped vanilla 320x200
                    // artwork to the complete physical LCD, including the
                    // rows normally reserved for the status bar.
                    pixel = frame_buffer[frame][screen_y * LCD_WIDTH + screen_x];
                } else if (state == GS_LEVEL && screen_y < LCD_WORLD_HEIGHT) {
                    // Fit the exact half-vanilla 160x84 viewport above the
                    // status bar. The endpoint-preserving 21:16 map includes
                    // both source rows 0 and 83.
                    _Static_assert(MAIN_VIEWHEIGHT == 84,
                                   "update the world row mapper");
                    int source_y = (screen_y * 21 + 8) >> 4;
                    pixel = frame_buffer[frame][source_y * LCD_WIDTH + screen_x];
                } else if (state == GS_LEVEL) {
                    int hud_x = screen_x * 2;
                    int hud_y = (screen_y - LCD_WORLD_HEIGHT) * 2;
                    pixel = status_buffer[frame][hud_y * 320 + hud_x];
                } else {
                    // TITLEPIC, CREDIT, and other full-screen pages retain all
                    // 80 native LCD rows.
                    pixel = frame_buffer[frame][screen_y * LCD_WIDTH + screen_x];
                }
                if (transition_frames) {
                    int phase = 20 - transition_frames;
                    int delay = ((screen_x * 13) ^ (screen_x >> 2)) % 5;
                    int reveal = (phase - delay) * 6;
                    if (reveal < 0) reveal = 0;
                    if (reveal > LCD_HEIGHT) reveal = LCD_HEIGHT;
                    if (screen_y >= reveal)
                        pixel = transition_frame[(screen_y - reveal) * LCD_WIDTH
                                                 + screen_x];
                }
                out[i] = palette565[pixel];
            }
            lcd_tx(dma_rows, sizeof(dma_rows), 1);
        }
        if (transition_frames) --transition_frames;
        sem_release(&display_frame_freed);
        ++frames;
        int64_t now = esp_timer_get_time();
        if (now >= report_at) {
            printf("LCD: %lu frames/5s (%.1f fps), state=%d, tics=%d, free=%u, largest=%u\n",
                   (unsigned long)frames, frames / 5.0,
                   (int)gamestate, gametic - last_gametic,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            frames = 0; last_gametic = gametic; report_at = now + 5000000;
        }
    }
}

void I_InitGraphics(void)
{
    sem_init(&render_frame_ready, 0, 2); sem_init(&display_frame_freed, 1, 2);
    frame_storage = heap_caps_calloc(1, FRAME_STORAGE_BYTES,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame_storage) abort();
    frame_buffer = (uint8_t (*)[SCREENWIDTH * MAIN_VIEWHEIGHT])
            (frame_storage + SCREENWIDTH * FRAME_PREFIX_ROWS);
    status_buffer = heap_caps_calloc(2, 320 * 32,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!status_buffer) abort();
    transition_frame = frame_storage + SCREENWIDTH *
            (FRAME_PREFIX_ROWS + 2 * MAIN_VIEWHEIGHT);
    pd_init();
    I_VideoBuffer = frame_buffer[0];
    // Load PLAYPAL before the first submitted frame; the 320x32 status bar is
    // composed separately and downsampled by the display task.
    build_palette(0);
    lcd_init(); show_poweron_pattern();
    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "doom-lcd", 3072, NULL, 6, NULL, 1);
    if (ok != pdPASS) abort();
    printf("LCD: ST7735 160x80, 40MHz SPI DMA, CPU1 producer-consumer\n");
}

void I_ShutdownGraphics(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_SetPaletteNum(int num) { build_palette(num); }
int I_GetPaletteIndex(int r, int g, int b) { return (r + g + b) / 3; }
void I_UpdateNoBlit(void) {}
void I_FinishUpdate(void) {}
void I_ReadScreen(pixel_t *scr) { memcpy(scr, I_VideoBuffer, SCREENWIDTH * MAIN_VIEWHEIGHT); }
void I_BeginRead(void) {}
void I_SetWindowTitle(const char *title) { (void)title; }
void I_CheckIsScreensaver(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_BindVideoVariables(void) {}
void I_InitWindowTitle(void) {}
void I_InitWindowIcon(void) {}
void I_StartFrame(void) {}
void I_StartTic(void) { I_GetEvent(); }
void I_EnableLoadingDisk(int x, int y) { (void)x; (void)y; }
void I_GetWindowPosition(int *x, int *y, int w, int h) { (void)w; (void)h; *x = 0; *y = 0; }
