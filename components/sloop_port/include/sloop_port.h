/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: SLOOP on the ESP32-S3 (the platform of sloop_platform.h on ESP-IDF, and the
 * two SLOOP tasks). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* where a rendered half buffer goes (an audio_backend); blocks until it can take more */
typedef esp_err_t (*sloop_port_audio_out_t)(const int32_t *lr24, size_t frames);

typedef struct {
    int core;                     /* both SLOOP tasks run here (the FM-1's ISR-over-main-loop model) */
    int audio_prio, ui_prio;
    uint32_t audio_stack, ui_stack;
    sloop_port_audio_out_t audio_out;
} sloop_port_config_t;

#define SLOOP_PORT_CONFIG_DEFAULT() { .core = 1, .audio_prio = 20, .ui_prio = 5, \
                                      .audio_stack = 6144, .ui_stack = 8192, .audio_out = NULL }

/* the store: the "sloop" data partition (SLOOP's FM-1 flash map 0x90000..0xFFFFF). Before SLOOP
 * starts, sloop_port_store_init also applies a backup staged by sloop_port_store_stage. */
esp_err_t sloop_port_store_init(void);
/* a backup to restore (SLOOP_STORE_BYTES, sloop_store_check-ed): written to the "stage" partition
 * with a CRC trailer; the next boot copies it into "sloop" before SLOOP starts (no race with
 * SLOOP's own writes). ESP_OK, or an error (nothing staged). */
esp_err_t sloop_port_store_stage(const uint8_t *img, size_t n);

/* creates the locks and the audio + UI tasks; the UI task boots SLOOP */
esp_err_t sloop_port_start(const sloop_port_config_t *cfg);
int sloop_port_booted(void);

typedef struct {
    uint32_t store_reads, store_erases, store_writes, store_errors;
    uint32_t audio_renders, render_max_us, ui_steps;
    uint32_t audio_stack_free, ui_stack_free;    /* high-water marks, bytes */
} sloop_port_stats_t;
void sloop_port_get_stats(sloop_port_stats_t *st);

#ifdef __cplusplus
}
#endif
