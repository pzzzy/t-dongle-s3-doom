// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

void D_DoomMain(void);
extern int myargc;
extern char **myargv;

void app_main(void) {
    static char *startup_args[] = {"doom", "-warp", "1", "1", NULL};
    myargc = 4;
    myargv = startup_args;
    printf("\nT-Dongle-S3 DOOM low-memory port\n");
    printf("free internal heap: %u, largest block: %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    D_DoomMain();
    abort();
}
