/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: host-only controls of the Linux SLOOP platform (platform_host.c). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clock: the virtual clock only moves through host_clock_advance_us() and sleeps */
void host_clock_virtual(int on);
void host_clock_advance_us(uint64_t us);

/* storage: path = a file (created erased, kept across runs), NULL = in memory (erased) */
int host_store_open(const char *path);
void host_store_close(void);
uint8_t *host_store_image(void);                 /* the raw window (SLOOP_STORE_SIZE bytes) */
void host_store_stats(uint32_t *reads, uint32_t *erases, uint32_t *writes);

void host_set_reboot_hook(void (*fn)(void));

#ifdef __cplusplus
}
#endif
