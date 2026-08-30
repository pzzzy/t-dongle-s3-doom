// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __not_in_flash_func
#define __not_in_flash_func(x) x
#endif
#ifndef __no_inline_not_in_flash_func
#define __no_inline_not_in_flash_func(x) __attribute__((noinline)) x
#endif
#ifndef __mul_instruction
#define __mul_instruction(a, b) ((a) * (b))
#endif
#ifndef hard_assert
#define hard_assert assert
#endif
#ifndef panic
#define panic(...) abort()
#endif

typedef struct {
    SemaphoreHandle_t handle;
    StaticSemaphore_t storage;
} semaphore_t;

static inline void sem_init(semaphore_t *s, int initial, int max_count) {
    s->handle = xSemaphoreCreateCountingStatic((UBaseType_t)max_count,
                                                (UBaseType_t)initial,
                                                &s->storage);
    assert(s->handle != NULL);
}
static inline int sem_available(semaphore_t *s) {
    return uxSemaphoreGetCount(s->handle) != 0;
}
static inline void sem_release(semaphore_t *s) {
    (void)xSemaphoreGive(s->handle);
}
static inline void sem_acquire_blocking(semaphore_t *s) {
    (void)xSemaphoreTake(s->handle, portMAX_DELAY);
}
static inline int sem_acquire_timeout_ms(semaphore_t *s, uint32_t ms) {
    return xSemaphoreTake(s->handle, pdMS_TO_TICKS(ms)) == pdTRUE;
}

typedef struct { int unused; } spin_lock_t;
static inline spin_lock_t *spin_lock_instance(unsigned int ignored) {
    (void)ignored;
    static spin_lock_t lock;
    return &lock;
}
static inline uint32_t spin_lock_blocking(spin_lock_t *lock) {
    (void)lock;
    return 0;
}
static inline void spin_unlock(spin_lock_t *lock, uint32_t state) {
    (void)lock;
    (void)state;
}

static inline uint32_t hw_divider_u32_quotient_inlined(uint32_t a, uint32_t b) {
    return b ? a / b : UINT32_MAX;
}
static inline int32_t hw_divider_s32_quotient_inlined(int32_t a, int32_t b) {
    return b ? a / b : INT32_MAX;
}

static inline unsigned int get_core_num(void) { return 0; }

#define CU_REGISTER_DEBUG_PINS(...)
#define CU_SELECT_DEBUG_PINS(...)
#define DEBUG_PINS_SET(...)
#define DEBUG_PINS_CLR(...)
