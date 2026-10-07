/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: SLOOP on the ESP32-S3 (N16R16) with a PCM5102A, controlled from a browser.
 *
 * Boot: report the chip / memory, open NVS and SLOOP's store partition, start the audio output
 * (I2S to the PCM5102A, or the null output under QEMU), start SLOOP's audio + UI tasks, the USB-MIDI
 * device (SLOOPY_USB_MIDI), then the network and web panel (hardware profile) or the self-test
 * (QEMU profile).
 */
#include <inttypes.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "audio_backend.h"
#include "sloop.h"
#include "sloop_port.h"
#include "selftest.h"
#include "usb_midi.h"
#include "web_server.h"

static const char *TAG = "sloopy";

#ifdef CONFIG_SLOOPY_QEMU_BUILD
#define SLOOPY_QEMU 1
#else
#define SLOOPY_QEMU 0
#endif
#ifdef CONFIG_SLOOPY_ENABLE_WEB
#define SLOOPY_WEB 1
#else
#define SLOOPY_WEB 0
#endif

extern int _ext_ram_bss_start, _ext_ram_bss_end;

static uint32_t s_boot;                          /* boots of this flash (NVS): "a restart happened" for clients */

static uint32_t count_boot(void)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("sloopy", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boots", &n);
        n++;
        nvs_set_u32(h, "boots", n);
        nvs_commit(h);
        nvs_close(h);
    }
    return n;
}

/* diagnostics: GET /api/status and a console line every 10 s (hardware tests H-7, H-12) */
static size_t status_json(char *buf, size_t cap)
{
    sloop_port_stats_t ps;
    audio_backend_stats_t as;
    sloop_status_t st;
    sloop_web_stats_t ws = {0};
    usb_midi_stats_t us;
    const esp_app_desc_t *app = esp_app_get_description();
    const char *ota_state = "none";
    int n;
    sloop_port_get_stats(&ps);
    audio_backend_get_stats(&as);
    sloop_status_get(&st);
    usb_midi_get_stats(&us);
#if CONFIG_SLOOPY_ENABLE_WEB
    sloop_web_get_stats(&ws);
    ota_state = sloop_ota_state();
#endif
    n = snprintf(buf, cap,
                 "{\"sloop\":\"%s\",\"upstream\":\"%s\",\"boot\":%lu,\"uptime_ms\":%lu,\"playing\":%u,\"bpm\":%d,"
                 "\"app\":{\"version\":\"%s\",\"built\":\"%s %s\",\"partition\":\"%s\",\"ota_state\":\"%s\","
                 "\"ota\":%d},"
                 "\"audio\":{\"output\":\"%s\",\"frames\":%llu,\"underruns\":%lu,\"late\":%lu,\"render_max_us\":%lu,"
                 "\"cpu_pct\":%u,\"peak\":%ld},"
                 "\"heap\":{\"internal_free\":%u,\"internal_min\":%u,\"psram_free\":%u},"
                 "\"stack_free\":{\"audio\":%lu,\"ui\":%lu},"
                 "\"store\":{\"reads\":%lu,\"erases\":%lu,\"writes\":%lu,\"errors\":%lu,\"backup\":%d},"
                 "\"web\":{\"panels\":%lu,\"http\":%lu,\"ws_messages\":%lu,\"rects\":%lu},"
                 "\"usb_midi\":{\"enabled\":%d,\"linked\":%d,\"links\":%lu,\"rx\":%lu,\"rx_sysex\":%lu,"
                 "\"rx_dropped\":%lu,\"tx\":%lu,\"tx_dropped\":%lu}}",
                 sloop_version(), sloop_upstream_commit(), (unsigned long)s_boot, (unsigned long)st.uptime_ms, st.playing,
                 st.bpm, app->version, app->date, app->time, esp_ota_get_running_partition()->label, ota_state,
                 SLOOPY_WEB, audio_backend_kind() == AUDIO_BACKEND_I2S ? "i2s" : "null", (unsigned long long)as.frames,
                 (unsigned long)as.underruns, (unsigned long)as.late, (unsigned long)ps.render_max_us,
                 (unsigned)(st.cpu_q8 * 100u / 256u), (long)as.peak,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)ps.audio_stack_free,
                 (unsigned long)ps.ui_stack_free, (unsigned long)ps.store_reads, (unsigned long)ps.store_erases,
                 (unsigned long)ps.store_writes, (unsigned long)ps.store_errors, SLOOPY_WEB, (unsigned long)ws.clients,
                 (unsigned long)ws.http_requests, (unsigned long)ws.ws_messages, (unsigned long)ws.rects,
                 us.enabled, us.linked, (unsigned long)us.n.links, (unsigned long)us.n.rx_packets,
                 (unsigned long)us.n.rx_sysex, (unsigned long)us.n.rx_dropped, (unsigned long)us.n.tx_packets,
                 (unsigned long)us.n.tx_dropped);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        sloop_port_stats_t ps;
        audio_backend_stats_t as;
        sloop_status_t st;
        usb_midi_stats_t us;
        vTaskDelay(pdMS_TO_TICKS(10000));
        sloop_port_get_stats(&ps);
        audio_backend_get_stats(&as);
        sloop_status_get(&st);
        usb_midi_get_stats(&us);
        ESP_LOGI(TAG, "stats: %s, cpu %u%%, render max %lu us, underruns %lu, heap int %u (min %u), stack free a/u %lu/%lu",
                 st.playing ? "playing" : "stopped", (unsigned)(st.cpu_q8 * 100u / 256u), (unsigned long)ps.render_max_us,
                 (unsigned long)as.underruns, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned long)ps.audio_stack_free,
                 (unsigned long)ps.ui_stack_free);
        if (us.enabled)
            ESP_LOGI(TAG, "usb midi: %s, rx %lu (sysex %lu, dropped %lu), tx %lu (dropped %lu)",
                     us.linked ? "linked" : "no host", (unsigned long)us.n.rx_packets, (unsigned long)us.n.rx_sysex,
                     (unsigned long)us.n.rx_dropped, (unsigned long)us.n.tx_packets, (unsigned long)us.n.tx_dropped);
    }
}

static void report_system(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "sloopy-sloop-32: %s on ESP32-S3 rev %d, %d cores, ESP-IDF %s", sloop_version(),
             chip.revision, chip.cores, esp_get_idf_version());
    ESP_LOGI(TAG, "upstream SLOOP %s, profile %s", sloop_upstream_commit(),
             SLOOPY_QEMU ? "qemu" : "hardware");
    ESP_LOGI(TAG, "firmware %s (%s %s), running from %s", esp_app_get_description()->version,
             esp_app_get_description()->date, esp_app_get_description()->time, esp_ota_get_running_partition()->label);
    ESP_LOGI(TAG, "flash %" PRIu32 " bytes, PSRAM %u bytes", flash_size, (unsigned)esp_psram_get_size());
    ESP_LOGI(TAG, "heap: internal free %u, PSRAM free %u; SLOOP .pool in PSRAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)((uintptr_t)&_ext_ram_bss_end - (uintptr_t)&_ext_ram_bss_start));
}

/* SLOOP's audio and UI are running (screens drawn, audio rendered), within timeout_ms */
static int sloop_runs(uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t += 50) {
        sloop_status_t st;
        sloop_status_get(&st);
        if (st.ui_frames > 0 && st.audio_halves > 0)
            return 1;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return 0;
}

void app_main(void)
{
    audio_backend_config_t audio = {
        .kind = SLOOPY_QEMU ? AUDIO_BACKEND_NULL : AUDIO_BACKEND_I2S,
        .sample_rate = CONFIG_SLOOPY_AUDIO_SAMPLE_RATE,
        .bclk_gpio = CONFIG_SLOOPY_I2S_BCLK_GPIO,
        .ws_gpio = CONFIG_SLOOPY_I2S_WS_GPIO,
        .dout_gpio = CONFIG_SLOOPY_I2S_DOUT_GPIO,
        .dma_desc_num = CONFIG_SLOOPY_I2S_DMA_DESC,
        .dma_frame_num = CONFIG_SLOOPY_I2S_DMA_FRAMES,
    };
    sloop_port_config_t port = SLOOP_PORT_CONFIG_DEFAULT();
    esp_err_t err;
    int updated = 0;                             /* the first boot of a firmware update */

    report_system();
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_boot = count_boot();

#if CONFIG_SLOOPY_ENABLE_WEB
    updated = sloop_ota_pending();
#endif
#if CONFIG_SLOOPY_SELFTEST
    if (!updated)
        selftest_before_boot();                  /* the cross-target render, before SLOOP boots */
#endif
    sloop_port_store_init();                     /* without it SLOOP runs RAM-only, as on a bad flash */
    ESP_ERROR_CHECK(audio_backend_init(&audio));
    port.core = CONFIG_SLOOPY_SLOOP_CORE;
    port.audio_out = audio_backend_write;
    ESP_ERROR_CHECK(sloop_port_start(&port));
#if CONFIG_SLOOPY_USB_MIDI
    if (usb_midi_start() != 0)
        ESP_LOGE(TAG, "USB-MIDI device not started");
#endif
#if CONFIG_SLOOPY_ENABLE_WEB
    sloop_web_set_status_fn(status_json);
    {
        int web_ok = sloop_net_start() == ESP_OK &&
                     sloop_web_start(SLOOPY_QEMU ? "esp32s3-qemu" : "esp32s3", CONFIG_SLOOPY_WEB_PORT) == ESP_OK;
        if (updated)                             /* keep it only if it can be reached and updated again */
            sloop_ota_confirm(web_ok && sloop_runs(10000));
    }
#endif
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 1, NULL, 0);
#if CONFIG_SLOOPY_SELFTEST
    if (updated)
        printf("SLOOPY_OTA_BOOT: self-test skipped on an update's first boot\n");
    else
        selftest_start();
#endif
    ESP_LOGI(TAG, "SLOOPY_APP_READY");
}
