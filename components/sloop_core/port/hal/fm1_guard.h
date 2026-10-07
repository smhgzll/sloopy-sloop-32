/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_guard.h).
 * The FM-1 stack / write / PC limit guards; on ESP32 the MPU, stack canaries and the
 * task watchdog of ESP-IDF take this role, on the host the OS does. */
#pragma once
#include <stdint.h>

enum { FM1_GUARD_STACK = 1, FM1_GUARD_WRITE = 2, FM1_GUARD_BUS = 4, FM1_GUARD_PC = 8 };
static inline void fm1_guard_enable(uint32_t which) { (void)which; }
static inline void fm1_guard_lock_top(void) {}
static inline void fm1_guard_unlock_top(void) {}
