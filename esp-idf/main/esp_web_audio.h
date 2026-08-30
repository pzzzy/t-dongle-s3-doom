// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

// Ordered binary messages on the control WebSocket. Publishing is bounded,
// allocation-free, and safe from the game task; packets are sent by HTTPD.
void esp_web_audio_publish(const uint8_t *message, size_t length);
void esp_web_audio_reset_transport(void);

// Implemented by the active sound backend. Called after a browser reconnects
// so it receives authoritative channel/music state rather than stale edges.
void esp_web_audio_sync_state(void);
