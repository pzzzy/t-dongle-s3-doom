// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "esp_err.h"

/* Starts the native USB-OTG keyboard host when CONFIG_TD_USB_HOST is enabled.
 * The board must be connected through a protected, externally powered host
 * adapter; the T-Dongle-S3 has no VBUS source switch of its own. */
esp_err_t esp_usb_hid_start(void);
