/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_sys.h).
 * Watchdog, reset reason and reboot of the JieLi chip; the update / UBOOT entries of the
 * FM-1 have no meaning on this target and reboot instead. */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"
#include "sloop_platform.h"

static struct {
    uint8_t p3_rst;      /* bit0 power-on (always, here) */
    uint32_t rst_src;
    uint8_t wdt_con;
} fm1_boot __attribute__((unused)) = {1u, 0u, 0u};

/* The FM-1's main loop feeds the watchdog, also inside the few loops that wait for the input
 * ISR (HARDWARE CALIBRATION). The port has no input ISR: feeding is where the port runtime
 * does that ISR's work (clock, input mailbox, screen publish) and yields. */
static void sloop_port_idle(void);
static inline void fm1_wdt_arm(uint8_t t) { (void)t; }
static inline void fm1_wdt_feed(void) { sloop_port_idle(); }
static inline void fm1_wdt_stop(void) {}
static inline void fm1_reset_reason(void) {}
static inline void fm1_reboot(void) { sloop_plat_reboot(); }
static inline void fm1_enter_uboot(void) { sloop_plat_reboot(); }
static inline void fm1_core_reset(void) { sloop_plat_reboot(); }
static inline void fm1_enter_update(const uint8_t *parm) { (void)parm; sloop_plat_reboot(); }
static inline void fm1_updata_parm_clear(void) {}
static inline void fm1_mailbox_clear(void) {}
