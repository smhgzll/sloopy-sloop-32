/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_timer.h).
 * TIMER5 drove the input scan, the USB poll and fm1_ms; the port runtime does that work
 * in sloop_ui_step(). */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

FM1_INLINE void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }
FM1_INLINE void fm1_timer5_ack(void) {}
