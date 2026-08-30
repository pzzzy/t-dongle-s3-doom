// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

enum {
    ESP_DOOM_CTL_FORWARD = 1u << 0,
    ESP_DOOM_CTL_BACK    = 1u << 1,
    ESP_DOOM_CTL_LEFT    = 1u << 2,
    ESP_DOOM_CTL_RIGHT   = 1u << 3,
    ESP_DOOM_CTL_FIRE    = 1u << 4,
    ESP_DOOM_CTL_USE     = 1u << 5,
    ESP_DOOM_CTL_RUN     = 1u << 6,
    ESP_DOOM_CTL_ENTER   = 1u << 7,
    ESP_DOOM_CTL_ESCAPE  = 1u << 8,
    ESP_DOOM_CTL_MAP     = 1u << 9,
    ESP_DOOM_CTL_STRAFE  = 1u << 10,
    ESP_DOOM_CTL_WEAPON1 = 1u << 11,
    ESP_DOOM_CTL_WEAPON2 = 1u << 12,
    ESP_DOOM_CTL_WEAPON3 = 1u << 13,
    ESP_DOOM_CTL_WEAPON4 = 1u << 14,
    ESP_DOOM_CTL_WEAPON5 = 1u << 15,
    ESP_DOOM_CTL_WEAPON6 = 1u << 16,
    ESP_DOOM_CTL_WEAPON7 = 1u << 17,
    ESP_DOOM_CTL_ALL     = (1u << 18) - 1,
};

// Remote producers submit a complete key-state snapshot. The game thread
// alone translates aggregate state transitions into D_PostEvent calls.
typedef enum {
    ESP_INPUT_SOURCE_WEB = 0,
    ESP_INPUT_SOURCE_USB,
    ESP_INPUT_SOURCE_COUNT,
} esp_input_source_t;

/* Every producer publishes a complete state snapshot.  This prevents a key-up
 * from one transport from cancelling the same key held by another transport. */
void esp_input_source_update(esp_input_source_t source, uint32_t mask);
void esp_input_source_disconnect(esp_input_source_t source);
int esp_input_source_active(esp_input_source_t source);

static inline void esp_input_remote_update(uint32_t mask)
{
    esp_input_source_update(ESP_INPUT_SOURCE_WEB, mask);
}

static inline void esp_input_remote_disconnect(void)
{
    esp_input_source_disconnect(ESP_INPUT_SOURCE_WEB);
}

static inline int esp_input_remote_active(void)
{
    return esp_input_source_active(ESP_INPUT_SOURCE_WEB);
}
