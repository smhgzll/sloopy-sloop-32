/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_irq.h).
 * "Interrupts off" in SLOOP means: the audio ISR must not run inside this section. The port
 * runs the audio ISR in its own context, so cli / sti become the platform's audio lock. */
#pragma once
#include <stdint.h>
#include "sloop_platform.h"

static inline void fm1_irq_off(void) { sloop_plat_audio_lock(); }
static inline void fm1_irq_on(void) { sloop_plat_audio_unlock(); }

#define FM1_CRASH_MAGIC 0x43525348u          /* "CRSH" */
typedef struct {
    uint32_t magic, count, vec, pc, rets, emu, dbg, sp, psr, icfg, uptime_ms;
    uint32_t etm[4];
    uint32_t early;
} fm1_crash_t;
static fm1_crash_t fm1_crash __attribute__((unused));              /* the port reports crashes through the platform (core dumps) */

static inline void fm1_irq_init(void) {}
static inline void fm1_irq_enable_all(void) {}
