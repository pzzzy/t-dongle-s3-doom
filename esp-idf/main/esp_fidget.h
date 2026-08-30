// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>

/* The GPIO button requests the mode; game-state changes are performed by the
 * game thread from the two ticker hooks. */
void esp_fidget_request_start(void);
bool esp_fidget_engaged(void);
bool esp_fidget_active(void);
unsigned esp_fidget_kills(void);
const char *esp_fidget_weapon_name(void);
void esp_fidget_pre_game_ticker(void);
void esp_fidget_level_ticker(void);
