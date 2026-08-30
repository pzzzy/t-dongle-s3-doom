// SPDX-License-Identifier: GPL-2.0-or-later
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#if !CONFIG_TD_USB_HOST
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "d_event.h"
#include "doomkeys.h"
#include "esp_check.h"
#include "esp_fidget.h"
#include "esp_timer.h"
#include "esp_input_cheat.h"
#include "esp_input_remote.h"
#include "esp_usb_hid.h"
#include "esp_web_control.h"
#include "i_input.h"

int novert;
static int initialized;
static uint32_t serial_mask;
static uint32_t posted_mask;
static int gpio_was_pressed;
static atomic_uint_fast32_t source_mask[ESP_INPUT_SOURCE_COUNT];
static atomic_uint_fast32_t source_last_ms[ESP_INPUT_SOURCE_COUNT];

#define CHEAT_QUEUE_CAPACITY 64u
static char cheat_queue[CHEAT_QUEUE_CAPACITY];
static atomic_uint cheat_head;
static atomic_uint cheat_tail;

#define REMOTE_INPUT_TIMEOUT_MS 1200u

typedef struct {
    char serial;
    uint32_t control;
    int doom_key;
} keymap_t;

static const keymap_t keymap[] = {
    {'w', ESP_DOOM_CTL_FORWARD, KEY_UPARROW},
    {'s', ESP_DOOM_CTL_BACK, KEY_DOWNARROW},
    {'a', ESP_DOOM_CTL_LEFT, KEY_LEFTARROW},
    {'d', ESP_DOOM_CTL_RIGHT, KEY_RIGHTARROW},
    {'j', ESP_DOOM_CTL_FIRE, KEY_RCTRL},
    {'e', ESP_DOOM_CTL_USE, ' '},
    {'k', ESP_DOOM_CTL_RUN, KEY_RSHIFT},
    {'q', ESP_DOOM_CTL_ENTER, KEY_ENTER},
    {'m', ESP_DOOM_CTL_ESCAPE, KEY_ESCAPE},
    {'o', ESP_DOOM_CTL_MAP, KEY_TAB},
    {'l', ESP_DOOM_CTL_STRAFE, KEY_RALT},
    {'1', ESP_DOOM_CTL_WEAPON1, '1'},
    {'2', ESP_DOOM_CTL_WEAPON2, '2'},
    {'3', ESP_DOOM_CTL_WEAPON3, '3'},
    {'4', ESP_DOOM_CTL_WEAPON4, '4'},
    {'5', ESP_DOOM_CTL_WEAPON5, '5'},
    {'6', ESP_DOOM_CTL_WEAPON6, '6'},
    {'7', ESP_DOOM_CTL_WEAPON7, '7'},
};

static void post_key(int key, int down)
{
    event_t event = {
        .type = down ? ev_keydown : ev_keyup,
        .data1 = key,
        .data2 = down && key < 128 ? key : 0,
        .data3 = down && key < 128 ? key : 0,
    };
    D_PostEvent(&event);
}

esp_err_t esp_input_queue_cheat(const char *text)
{
    size_t length = strlen(text);
    if (!length || length > 16) return ESP_ERR_INVALID_ARG;

    unsigned head = atomic_load_explicit(&cheat_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&cheat_tail, memory_order_acquire);
    unsigned used = head - tail;
    if (length > CHEAT_QUEUE_CAPACITY - used) return ESP_ERR_NO_MEM;

    for (size_t i = 0; i < length; ++i) {
        unsigned char key = (unsigned char)text[i];
        if (!islower(key) && !isdigit(key)) return ESP_ERR_INVALID_ARG;
        cheat_queue[(head + i) % CHEAT_QUEUE_CAPACITY] = (char)key;
    }
    atomic_store_explicit(&cheat_head, head + length, memory_order_release);
    return ESP_OK;
}

static void post_queued_cheat_key(void)
{
    unsigned tail = atomic_load_explicit(&cheat_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&cheat_head, memory_order_acquire);
    if (tail == head) return;

    int key = (unsigned char)cheat_queue[tail % CHEAT_QUEUE_CAPACITY];
    post_key(key, 1);
    post_key(key, 0);
    atomic_store_explicit(&cheat_tail, tail + 1, memory_order_release);
}

static uint32_t input_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void esp_input_source_update(esp_input_source_t source, uint32_t mask)
{
    if ((unsigned)source >= ESP_INPUT_SOURCE_COUNT) return;
    atomic_store_explicit(&source_mask[source], mask & ESP_DOOM_CTL_ALL,
                          memory_order_relaxed);
    atomic_store_explicit(&source_last_ms[source], input_time_ms(),
                          memory_order_release);
}

void esp_input_source_disconnect(esp_input_source_t source)
{
    if ((unsigned)source >= ESP_INPUT_SOURCE_COUNT) return;
    atomic_store_explicit(&source_mask[source], 0, memory_order_relaxed);
    atomic_store_explicit(&source_last_ms[source], 0, memory_order_release);
}

int esp_input_source_active(esp_input_source_t source)
{
    if ((unsigned)source >= ESP_INPUT_SOURCE_COUNT) return 0;
    uint32_t last = atomic_load_explicit(&source_last_ms[source],
                                         memory_order_acquire);
    if (!last) return 0;
    /* Web clients send heartbeats, while USB HID reports are edge-driven and
     * remain authoritative until an explicit zero report or disconnect. */
    return source == ESP_INPUT_SOURCE_USB ||
           input_time_ms() - last <= REMOTE_INPUT_TIMEOUT_MS;
}

static uint32_t current_source_mask(esp_input_source_t source)
{
    if (!esp_input_source_active(source)) {
        atomic_store_explicit(&source_mask[source], 0, memory_order_relaxed);
        return 0;
    }
    return atomic_load_explicit(&source_mask[source], memory_order_relaxed);
}

static void post_mask_changes(uint32_t desired)
{
    uint32_t changed = posted_mask ^ desired;
    if (!changed) return;

    for (unsigned i = 0; i < sizeof(keymap) / sizeof(keymap[0]); ++i) {
        if (changed & keymap[i].control)
            post_key(keymap[i].doom_key, !!(desired & keymap[i].control));
    }
    posted_mask = desired;
}

static void input_init_once(void)
{
    if (initialized) return;
    initialized = 1;
#if !CONFIG_TD_USB_HOST
    usb_serial_jtag_driver_config_t usb_config =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&button);
    printf("INPUT: source-safe serial/GPIO/web/USB broker; GPIO0=fire\n");

    esp_err_t web_result = esp_web_control_start();
    if (web_result != ESP_OK)
        printf("WEB: disabled (%s); local controls remain available\n",
               esp_err_to_name(web_result));

    esp_err_t usb_result = esp_usb_hid_start();
    if (usb_result != ESP_OK && usb_result != ESP_ERR_NOT_SUPPORTED)
        printf("USB HID: disabled (%s); other controls remain available\n",
               esp_err_to_name(usb_result));
}

void I_BindInputVariables(void) {}
void I_ReadMouse(void) {}
void I_StartTextInput(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void I_StopTextInput(void) {}
void I_InputInit(void) { input_init_once(); }

void I_GetEvent(void) { I_GetEventTimeout(0); }

void I_GetEventTimeout(int timeout)
{
    (void)timeout;
    input_init_once();
    // One character per poll prevents the tiny eight-entry event queue from
    // being overwritten by long sequences such as IDSPISPOPD.
    post_queued_cheat_key();
    unsigned char bytes[32];
#if CONFIG_TD_USB_HOST
    ssize_t count = 0;
#else
    ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
#endif
    for (ssize_t i = 0; i < count; ++i) {
        int down = !isupper(bytes[i]);
        char code = (char)tolower(bytes[i]);
        if (down && code == 'h') {
            esp_fidget_request_start();
            continue;
        }
        for (unsigned j = 0; j < sizeof(keymap) / sizeof(keymap[0]); ++j) {
            if (keymap[j].serial == code) {
                if (down) serial_mask |= keymap[j].control;
                else serial_mask &= ~keymap[j].control;
                break;
            }
        }
    }

    int gpio_pressed = gpio_get_level(0) == 0;
    if (gpio_pressed && !gpio_was_pressed && !esp_fidget_engaged())
        esp_fidget_request_start();
    gpio_was_pressed = gpio_pressed;
    uint32_t gpio_mask = gpio_pressed && esp_fidget_active()
                       ? ESP_DOOM_CTL_FIRE : 0;
    uint32_t sources = 0;
    for (esp_input_source_t source = 0; source < ESP_INPUT_SOURCE_COUNT;
         source++)
        sources |= current_source_mask(source);
    post_mask_changes(serial_mask | gpio_mask | sources);
}
