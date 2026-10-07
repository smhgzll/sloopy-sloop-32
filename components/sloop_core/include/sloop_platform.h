/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: what a target must provide to the SLOOP core.
 *
 * The SLOOP sources (upstream/sloop-fm1/firmware/src) are compiled unmodified against the
 * virtual FM-1 HAL in components/sloop_core/port/hal. That HAL only calls the functions
 * below, so the core never touches ESP-IDF, POSIX, GPIO, I2S or Wi-Fi APIs directly.
 *
 * Implementations: host/platform_host.c (Linux), components/sloop_platform_esp32 (ESP-IDF).
 *
 * Contexts:
 *   UI context     the task / thread that calls sloop_boot() and sloop_ui_step() (the FM-1 main loop)
 *   audio context  the task / thread that calls sloop_audio_render() (the FM-1 audio ISR)
 *   any            transport tasks (WebSocket, MIDI, tests) using the sloop_post_* / sloop_display_*
 *                  mailboxes in sloop.h
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clock */
uint64_t sloop_plat_time_us(void);          /* monotonic microseconds since an arbitrary origin */
void sloop_plat_sleep_us(uint32_t us);      /* UI context: yield for at least us */

/* ---- the FM-1's "interrupts off": a critical section of the UI context against the audio
 * context. Non-nesting, like cli / sti: lock twice + unlock once releases. A call from the
 * audio context is a no-op (the ISR cannot be preempted by itself). */
void sloop_plat_audio_lock(void);
void sloop_plat_audio_unlock(void);
/* the audio context brackets each render with these (the ISR entry and exit): begin waits
 * until the UI context is out of its critical section, and keeps it out until end */
void sloop_plat_render_begin(void);
void sloop_plat_render_end(void);

/* ---- a short lock for the I/O mailboxes (input queue, display / LED / status snapshots)
 * shared between the UI context and transport tasks. Never held across SLOOP code. */
void sloop_plat_io_lock(void);
void sloop_plat_io_unlock(void);

/* ---- persistent storage. Offsets are SLOOP's FM-1 flash addresses inside
 * [SLOOP_STORE_LO, SLOOP_STORE_HI) (settings, projects, user presets, user samples; see
 * upstream storage.c). Erase granularity 4 KiB, erased bytes read 0xFF, programming can
 * only clear bits (NOR semantics; storage.c relies on erase-before-write only).
 * Return 0 on success, < 0 on failure. */
#define SLOOP_STORE_LO 0x00090000u
#define SLOOP_STORE_HI 0x00100000u
#define SLOOP_STORE_SIZE (SLOOP_STORE_HI - SLOOP_STORE_LO)
int sloop_plat_store_ok(void);
int sloop_plat_store_read(uint32_t off, void *dst, uint32_t n);
int sloop_plat_store_erase(uint32_t off, uint32_t n);
int sloop_plat_store_write(uint32_t off, const void *src, uint32_t n);
/* memory-mapped read pointer of off (user sample sets are played from it), NULL if unmapped */
const uint8_t *sloop_plat_store_ptr(uint32_t off);

/* ---- system */
void sloop_plat_reboot(void);
void sloop_plat_log(const char *msg);

#ifdef __cplusplus
}
#endif
