// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "esp_err.h"

// Queue a validated cheat string for delivery by the game thread. This keeps
// D_PostEvent single-threaded even though HTTP requests run on another task.
esp_err_t esp_input_queue_cheat(const char *text);
