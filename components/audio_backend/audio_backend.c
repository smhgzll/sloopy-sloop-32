/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: audio backends (see audio_backend.h). */
#include "audio_backend.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

static audio_backend_config_t s_cfg;
static audio_backend_kind_t s_kind = AUDIO_BACKEND_NULL;
static audio_backend_stats_t s_st;
static int64_t s_t0;                      /* NULL backend: clock of frame 0 */
static i2s_chan_handle_t s_tx;
/* I2S staging: 32-bit slots, left-justified 24-bit; in internal RAM (DMA source) */
#define STAGE_FRAMES 256u
static DRAM_ATTR int32_t s_stage[2u * STAGE_FRAMES];

/* the DMA had nothing new to send (auto_clear sends silence): a real underrun (hardware test H-7);
 * counted from the first write (before it, while SLOOP boots, the DMA plays its cleared buffers) */
static volatile uint32_t s_underruns;
static volatile bool s_writing;
static bool IRAM_ATTR on_send_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle; (void)event; (void)user_ctx;
    if (s_writing)
        s_underruns++;
    return false;
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_cfg.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                     /* PCM5102A: SCK tied low, PLL from BCK */
            .bclk = (gpio_num_t)s_cfg.bclk_gpio,
            .ws = (gpio_num_t)s_cfg.ws_gpio,
            .dout = (gpio_num_t)s_cfg.dout_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    esp_err_t err;
    chan.dma_desc_num = s_cfg.dma_desc_num;
    chan.dma_frame_num = s_cfg.dma_frame_num;
    chan.auto_clear = true;                              /* underrun / flash write: silence, not a loop */
    err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK)
        return err;
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) {
        i2s_event_callbacks_t cbs = {.on_send_q_ovf = on_send_q_ovf};
        err = i2s_channel_register_event_callback(s_tx, &cbs, NULL);
    }
    if (err == ESP_OK)
        err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return err;
}

esp_err_t audio_backend_init(const audio_backend_config_t *cfg)
{
    s_cfg = *cfg;
    memset(&s_st, 0, sizeof s_st);
    s_st.hash = 0xCBF29CE484222325ull;
    s_kind = AUDIO_BACKEND_NULL;
    if (cfg->kind == AUDIO_BACKEND_I2S) {
        if (cfg->bclk_gpio < 0 || cfg->ws_gpio < 0 || cfg->dout_gpio < 0) {
            ESP_LOGW(TAG, "I2S pins not configured (BCLK %d, WS %d, DOUT %d): no audio output, "
                          "set them in config/local.env", cfg->bclk_gpio, cfg->ws_gpio, cfg->dout_gpio);
        } else {
            esp_err_t err = i2s_init();
            if (err == ESP_OK) {
                s_kind = AUDIO_BACKEND_I2S;
                ESP_LOGI(TAG, "I2S TX: %u Hz, 32-bit stereo, BCLK %d, WS %d, DOUT %d, DMA %u x %u frames",
                         (unsigned)cfg->sample_rate, cfg->bclk_gpio, cfg->ws_gpio, cfg->dout_gpio,
                         (unsigned)cfg->dma_desc_num, (unsigned)cfg->dma_frame_num);
                return ESP_OK;
            }
            ESP_LOGE(TAG, "I2S init failed: %s; no audio output", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "null audio output (paced at %u Hz)", (unsigned)cfg->sample_rate);
    s_t0 = esp_timer_get_time();
    return ESP_OK;
}

audio_backend_kind_t audio_backend_kind(void) { return s_kind; }

static void account(const int32_t *lr24, size_t frames)
{
    size_t i;
    uint64_t h = s_st.hash;
    for (i = 0; i < frames; i++) {
        int32_t l = lr24[2u * i], r = lr24[2u * i + 1u];
        int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > s_st.peak) s_st.peak = al;
        if (ar > s_st.peak) s_st.peak = ar;
        if (l || r) s_st.nonzero_frames++;
        if (s_kind == AUDIO_BACKEND_NULL) {
            uint32_t u = (uint32_t)l & 0xFFFFFFu, v = (uint32_t)r & 0xFFFFFFu;
            h = (h ^ (u & 0xFFu)) * 0x100000001B3ull; h = (h ^ ((u >> 8) & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ (u >> 16)) * 0x100000001B3ull;
            h = (h ^ (v & 0xFFu)) * 0x100000001B3ull; h = (h ^ ((v >> 8) & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ (v >> 16)) * 0x100000001B3ull;
        }
    }
    s_st.hash = h;
}

esp_err_t audio_backend_write(const int32_t *lr24, size_t frames)
{
    int64_t t0 = esp_timer_get_time();
    account(lr24, frames);
    if (s_kind == AUDIO_BACKEND_I2S) {
        size_t done = 0;
        while (done < frames) {
            size_t n = frames - done > STAGE_FRAMES ? STAGE_FRAMES : frames - done, i, wrote = 0;
            for (i = 0; i < 2u * n; i++)
                s_stage[i] = lr24[2u * done + i] << 8;   /* 24-bit -> left-justified 32-bit slot */
            i2s_channel_write(s_tx, s_stage, n * 2u * sizeof(int32_t), &wrote, portMAX_DELAY);
            done += n;
        }
        s_writing = true;
    } else {
        /* paced by the clock: this block plays at frame s_st.frames; keep two blocks of lead,
         * like a DMA ring, and count a block as late when the clock is already past it */
        int64_t due = s_t0 + (int64_t)(s_st.frames * 1000000u / s_cfg.sample_rate);
        int64_t lead = (int64_t)(2u * frames * 1000000u / s_cfg.sample_rate);
        int64_t now = esp_timer_get_time();
        if (now > due + lead)
            s_st.late++;
        else if (due - lead > now)
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((due - lead - now) / 1000)) + 1u);
        else
            taskYIELD();
        if (now > due + 50 * lead)                       /* far behind (an emulator): no catch-up storm */
            s_t0 = now - (int64_t)(s_st.frames * 1000000u / s_cfg.sample_rate);
    }
    s_st.frames += frames;
    {
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_st.write_max_us)
            s_st.write_max_us = us;
    }
    return ESP_OK;
}

void audio_backend_get_stats(audio_backend_stats_t *st)
{
    *st = s_st;
    st->underruns = s_underruns;
}
