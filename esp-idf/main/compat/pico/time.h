// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "esp_doom_compat.h"
#include "esp_timer.h"

static inline uint64_t time_us_64(void) { return (uint64_t)esp_timer_get_time(); }
static inline uint32_t time_us_32(void) { return (uint32_t)esp_timer_get_time(); }
