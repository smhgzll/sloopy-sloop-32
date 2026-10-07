/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the two SLOOP tasks on the ESP32-S3.
 *
 *   audio task  (high priority) renders a half buffer (256 frames, 5.8 ms) and hands it to the
 *               audio output, which blocks until the DMA can take it: the FM-1's ALNK0 ISR.
 *   UI task     (low priority, same core) boots SLOOP and runs its main loop (sloop_ui_step,
 *               ~1 ms steps, 15 ms frames): input, LEDs, screen, autosave.
 * Transports (web server, MIDI) run elsewhere and only use sloop.h's mailboxes.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sloop.h"
#include "sloop_port.h"
#include "sloop_port_priv.h"

static const char *TAG = "sloop_port";
static sloop_port_config_t s_cfg;
static TaskHandle_t s_audio_task, s_ui_task;
static volatile int s_booted;

static void audio_task(void *arg)
{
    (void)arg;
    while (!s_booted)                            /* (the ISR is attached by sloop_boot: audio_init) */
        vTaskDelay(1);
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        const int32_t *half = sloop_audio_render();
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        sloop_port_stats.audio_renders++;
        if (us > sloop_port_stats.render_max_us)
            sloop_port_stats.render_max_us = us;
        if (s_cfg.audio_out)
            s_cfg.audio_out(half, SLOOP_HALF_FRAMES);
        else
            vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "booting %s (upstream %s)", sloop_version(), sloop_upstream_commit());
    sloop_boot();
    s_booted = 1;
    ESP_LOGI(TAG, "SLOOP_BOOTED");
    for (;;) {
        uint32_t ms = sloop_ui_step();
        sloop_port_stats.ui_steps++;
        vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
    }
}

esp_err_t sloop_port_start(const sloop_port_config_t *cfg)
{
    s_cfg = *cfg;
    sloop_plat_locks_init();
    if (xTaskCreatePinnedToCore(audio_task, "sloop_audio", cfg->audio_stack, NULL, cfg->audio_prio, &s_audio_task,
                                cfg->core) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(ui_task, "sloop_ui", cfg->ui_stack, NULL, cfg->ui_prio, &s_ui_task, cfg->core) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

int sloop_port_booted(void) { return s_booted; }

void sloop_port_get_stats(sloop_port_stats_t *st)
{
    *st = sloop_port_stats;
    st->audio_stack_free = s_audio_task ? uxTaskGetStackHighWaterMark(s_audio_task) : 0;
    st->ui_stack_free = s_ui_task ? uxTaskGetStackHighWaterMark(s_ui_task) : 0;
}
