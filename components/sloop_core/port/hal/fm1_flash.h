/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_flash.h).
 *
 * SLOOP keeps its settings, projects, user presets and user samples at fixed offsets of the
 * FM-1's 1 MiB SPI NOR (upstream storage.c, eng_sample.c, upreset.c). The port keeps that map
 * and its A/B torn-write safety unchanged and backs the window [SLOOP_STORE_LO, SLOOP_STORE_HI)
 * with the platform's storage (an ESP32 flash partition, a file on the host). The FM-1's RAM
 * flash driver (SFC / XIP switching, cache invalidation, the JEDEC id) is not needed: the
 * entry points keep their names and contracts.
 */
#pragma once
#include <stdint.h>
#include "fm1_irq.h"
#include "sloop_platform.h"

#define FL_DATA_LO      0x00097000u                /* SLOOP's main store */
#define FL_DATA_HI      0x000E0000u
#define FL_GLOB_LO      0x000FC000u                /* superblock / globals */
#define FL_GLOB_HI      0x000FF000u
#define FL_OTA_LO       0x000E0000u                /* (FM-1 update staging: unused by the port) */
#define FL_OTA_HI       0x000E5000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI))
#ifndef FL_RANGE_OK
#define FL_RANGE_OK(off, n) FL_STORE_OK(off, n)
#endif
#define FL_JEDEC_SLOOP  0x856014u                  /* the id persist_boot() expects of a usable store */

#define FL_FAR(fn) (fn)                            /* (no XIP / RAM call range on this target) */

static inline uint32_t irq_save(void) { fm1_irq_off(); return 0; }
static inline void irq_restore(uint32_t f) { (void)f; fm1_irq_on(); }

static uint32_t fl_jedec_ram(void) { return sloop_plat_store_ok() ? FL_JEDEC_SLOOP : 0u; }

static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n)
{
    if (!FL_IN(off, n, SLOOP_STORE_LO, SLOOP_STORE_HI))
        return -1;
    return sloop_plat_store_read(off, dst, n) ? -2 : 0;
}

static int fl_erase4k_ram(uint32_t off, uint32_t *took_us)
{
    *took_us = 0;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -1;
    return sloop_plat_store_erase(off, 0x1000u) ? -2 : 0;
}

static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *took_us)
{
    *took_us = 0;
    if (!FL_RANGE_OK(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;                                 /* one page, no wrap (as the NOR driver) */
    return sloop_plat_store_write(off, src, n) ? -2 : 0;
}

static inline void fl_plain_window_init(void) {}
static inline void fl_inval(uint32_t off, uint32_t len) { (void)off; (void)len; }   /* coherent mapping */

/* The port does not take the audio lock around the flash itself: the platform serialises
 * flash access (on the ESP32 a flash write stalls both cores, the I2S driver plays silence
 * meanwhile, as the FM-1 silences its DMA buffer) and SLOOP only saves when quiet. */
static int fl_erase4k(uint32_t off, uint32_t *took_us) { return fl_erase4k_ram(off, took_us); }

static int fl_write(uint32_t off, const uint8_t *src_ram, uint32_t n)
{
    uint32_t took;
    while (n) {
        uint32_t chunk = 256u - (off & 0xFFu);
        int rc;
        if (chunk > n) chunk = n;
        rc = fl_prog_ram(off, src_ram, chunk, &took);
        if (rc) return rc;
        off += chunk; src_ram += chunk; n -= chunk;
    }
    return 0;
}

static int fl_verify_xip(uint32_t off, const uint8_t *ref, uint32_t n)
{
    const uint8_t *p = sloop_plat_store_ptr(off);
    uint32_t i;
    if (!p)
        return -1;
    for (i = 0; i < n; i++) if (p[i] != ref[i]) return -(int)(i + 1);
    return 0;
}
