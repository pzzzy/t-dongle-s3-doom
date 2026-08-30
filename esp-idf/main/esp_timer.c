// SPDX-License-Identifier: GPL-2.0-or-later
#include "i_timer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int I_GetTime(void) {
    return (int)((esp_timer_get_time() * TICRATE) / 1000000LL);
}

int I_GetTimeMS(void) {
    return (int)(esp_timer_get_time() / 1000LL);
}

void I_Sleep(int ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void I_WaitVBL(int count) {
    I_Sleep((count * 1000) / 70);
}

void I_InitTimer(void) {}
