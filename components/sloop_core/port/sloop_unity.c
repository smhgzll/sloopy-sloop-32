/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: SLOOP as one compilation unit, as upstream builds it (firmware/src/felucca.c),
 * against the virtual FM-1 HAL in port/hal instead of the JieLi registers in firmware/hal.
 *
 * Every upstream source below is included unmodified and in felucca.c's order. Two upstream
 * files are hardware glue and are replaced:
 *   usb.c   the FM-1's register-level USB-MIDI device  -> sloop_usb_shim.c (same rings / API)
 *   main.c  the FM-1's boot, interrupt wiring and loop  -> sloop_runtime.c (sloop.h API)
 * and these are not built: ota.c, recovery.c (the FM-1's firmware update over M-UPGRADE and its
 * USB recovery), console.c (the CDC serial console), midi_uart.c (TRS MIDI, off upstream too).
 *
 * Build options (as felucca.c; the port fixes the ones that depend on FM-1 hardware):
 *   FELUCCA_FLASH 1   settings, presets, projects, user samples in the platform store
 *   FELUCCA_OTA   0   no M-UPGRADE; the web editor protocol (editor.c) is built on its own
 *   FELUCCA_CDC   0   no USB serial console
 *   FELUCCA_UART  0   no TRS MIDI
 */
#include <stdint.h>
#include <string.h>
#include "sloop.h"
#include "sloop_platform.h"

#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "felucca_tables.h"

/* libc.c defines memset / memcpy / memcmp for the freestanding FM-1 build; here the C library
 * has them, so its copies get other names (as upstream's host tests do) */
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp

/* user sample slots: read through the platform's mapping of the store instead of the FM-1's
 * XIP window (eng_sample.c hook) */
#define SMP_USER_XIP(k) sloop_plat_store_ptr(SMP_USER_BASE + (k) * SMP_USER_SIZE)

#include "lcd.c"
#include "gfx.c"
#include "core.h"
#include "engines.c"
#include "drums.c"
#include "params.c"
#include "voice.c"
#include "slicer.c"
#include "fx.c"
#define FELUCCA_OTA 0
#define FELUCCA_CDC 0
#include "sloop_usb_shim.c"      /* (usb.c) */
#define FELUCCA_UART 0
#define FELUCCA_ARRANGER 1
#include "arranger.c"
#include "seq.c"
#include "audio.c"
#include "panel.c"
#include "ui.c"
#include "ui_song.c"
#include "ui_studio.c"
#include "icons.c"
#include "ui_draw.c"
#include "ui_layers.c"
#include "ui_menu.c"
#include "ui_input.c"
#define FELUCCA_FLASH 1
#include "fm1_flash.h"
static uint8_t flash_ok;
/* storage.c's three flash hooks (felucca.c defines them on the FM-1) */
static int st_read(uint32_t off, void *dst, uint32_t n) { return fl_read_ram(off, (uint8_t *)dst, n); }
static int st_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_STORE_OK(off, 0x1000u))
        return -8;
    return fl_erase4k_ram(off, &took);
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n))
        return -8;
    return fl_write(off, (const uint8_t *)src, n);
}
/* editor.c erases user sample sectors through felucca.c's quiet erase */
static int fl_erase4k_quiet(uint32_t off, uint32_t *took) { return fl_erase4k_ram(off, took); }
#include "storage.c"
#include "upreset.c"
#include "project.c"
#include "editor.c"              /* the web editor's SysEx protocol (felucca.c: with FELUCCA_OTA) */
#include "splash.c"
#include "sloop_runtime.c"       /* (main.c) */
#include "sloop_selftest.c"      /* (deterministic cross-target render) */
