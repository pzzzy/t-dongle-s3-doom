// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "m_misc.h"
#include "esp_heap_caps.h"

void I_AtExit(atexit_func_t func, boolean run_on_error) { (void)func; (void)run_on_error; }
void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

byte *I_ZoneBase(int *size) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Reserve enough contiguous DRAM for the guarded 160x80 two-page video
    // arena (allocated after the Doom zone), the LCD task stack, and IDF.
    const size_t runtime_reserve = 96 * 1024;
    size_t wanted = largest > runtime_reserve ? largest - runtime_reserve : largest;
    wanted &= ~3u;
    byte *zone = heap_caps_malloc(wanted, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!zone) I_Error("zone allocation failed (%u)", (unsigned)wanted);
    *size = (int)wanted;
    printf("zone memory: %p, %u bytes\n", zone, (unsigned)wanted);
    return zone;
}

void I_PrintBanner(const char *msg) { puts(msg); }
void I_PrintDivider(void) { puts("----------------------------------------"); }
void I_PrintStartupBanner(const char *description) { I_PrintDivider(); puts(description); I_PrintDivider(); }
void I_Init(void) {}
void I_Quit(void) { abort(); }
void I_Error(const char *error, ...) {
    va_list args;
    va_start(args, error);
    vprintf(error, args);
    va_end(args);
    putchar('\n');
    abort();
}
void *I_Realloc(void *ptr, size_t size) { return realloc(ptr, size); }
boolean I_GetMemoryValue(unsigned int offset, void *value, int size) {
    (void)offset; (void)value; (void)size; return false;
}
void I_BindVariables(void) {}
void I_Endoom(byte *endoom_data) { (void)endoom_data; }
void I_StartMultiTicTimer(void) {}
void I_StartDisplay(void) {}
