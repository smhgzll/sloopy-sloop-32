/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_time.h).
 * The FM-1 time base is TIMER4 at 24 MHz; here one tick is one microsecond of the
 * platform clock. Callers only use differences (unsigned, wrapping), as upstream. */
#pragma once
#include <stdint.h>
#include "sloop_platform.h"

#define FM1_TICKS_PER_US 1u

static volatile uint32_t fm1_ms;          /* (tentative: core.h defines it; the port runtime advances it) */

static inline void fm1_time_init(void) {}
static inline uint32_t fm1_ticks(void) { return (uint32_t)sloop_plat_time_us(); }
static inline uint32_t fm1_micros(void) { return (uint32_t)sloop_plat_time_us(); }

/* the FM-1 busy-waits while its timer ISR moves fm1_ms on; the port sleeps and moves it itself
 * (delays are only called from the UI context) */
static inline void fm1_delay_us(uint32_t us)
{
    sloop_plat_sleep_us(us);
    fm1_ms = (uint32_t)(sloop_plat_time_us() / 1000u);
}
static inline void fm1_delay_ms(uint32_t ms) { fm1_delay_us(ms * 1000u); }
