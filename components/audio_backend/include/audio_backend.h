/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: where SLOOP's audio goes on the ESP32-S3.
 *
 * SLOOP renders interleaved stereo int32 blocks with 24-bit signed samples (sloop.h). A backend
 * takes a block and blocks the caller (the audio task) until it can take the next one, which
 * paces the renders at the sample rate.
 *
 *   AUDIO_BACKEND_I2S   I2S standard (Philips) TX, 32-bit slots, to a PCM5102A (or any I2S DAC)
 *   AUDIO_BACKEND_NULL  no output, paced by the clock (QEMU, boards without a DAC); keeps a
 *                       hash and the peak of what it was given, for tests
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AUDIO_BACKEND_NULL, AUDIO_BACKEND_I2S } audio_backend_kind_t;

typedef struct {
    audio_backend_kind_t kind;
    uint32_t sample_rate;
    int bclk_gpio, ws_gpio, dout_gpio;   /* I2S pins; -1 = not set (falls back to NULL) */
    uint32_t dma_desc_num;               /* I2S DMA buffers */
    uint32_t dma_frame_num;              /* frames per DMA buffer */
} audio_backend_config_t;

esp_err_t audio_backend_init(const audio_backend_config_t *cfg);
esp_err_t audio_backend_write(const int32_t *lr24, size_t frames);   /* blocks */
audio_backend_kind_t audio_backend_kind(void);

typedef struct {
    uint64_t frames;                     /* taken so far */
    uint32_t late;                       /* blocks that arrived after the output needed them */
    uint32_t underruns;                  /* I2S: DMA buffers sent with no fresh audio (on_send_q_ovf) */
    uint32_t write_max_us;
    int32_t peak;                        /* largest |sample| (24-bit) */
    uint64_t nonzero_frames;
    uint64_t hash;                       /* FNV-1a over the 24-bit samples (NULL backend) */
} audio_backend_stats_t;
void audio_backend_get_stats(audio_backend_stats_t *st);

#ifdef __cplusplus
}
#endif
