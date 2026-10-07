/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: sloop_platform.h on ESP-IDF.
 *
 *   clock    esp_timer (µs) from SLOOP's first use at boot; sleeps are vTaskDelay (CONFIG_FREERTOS_HZ=1000)
 *   locks    FreeRTOS mutexes (priority inheritance). The audio lock is what "interrupts off"
 *            means to SLOOP: the UI task's critical sections and the audio task's renders
 *            exclude each other. Both SLOOP tasks run on one core with the audio task above the
 *            UI task, so the UI never observes half a render (the FM-1's ISR model).
 *   storage  the "sloop" data partition, SLOOP's FM-1 offsets minus SLOOP_STORE_LO; reads through
 *            esp_partition_read, user samples played through an esp_partition_mmap window
 *            (ESP-IDF flushes mapped cache lines after a write or erase); a restored backup
 *            arrives through the "stage" partition and is copied in at boot, before SLOOP starts
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sloop.h"
#include "sloop_platform.h"
#include "sloop_port.h"
#include "sloop_port_priv.h"

static const char *TAG = "sloop_plat";

/* ---------------------------------------------------------------- clock --- */
/* from this boot, as the FM-1's timer (SLOOP waits "20 s after boot" for its first autosave): a
 * chip reset clears the systimer, Espressif's QEMU keeps it across esp_restart. The first call is
 * in app_main, before any task. */
static int64_t s_t0_us = -1;
uint64_t sloop_plat_time_us(void)
{
    int64_t now = esp_timer_get_time();
    if (s_t0_us < 0)
        s_t0_us = now;
    return (uint64_t)(now - s_t0_us);
}

void sloop_plat_sleep_us(uint32_t us)
{
    TickType_t t = pdMS_TO_TICKS((us + 999u) / 1000u);
    vTaskDelay(t ? t : 1);
}

/* ---------------------------------------------------------------- locks --- */
static SemaphoreHandle_t s_audio_mx, s_io_mx;
static volatile TaskHandle_t s_holder;           /* UI-side holder of the audio lock */
static volatile TaskHandle_t s_rendering;        /* the audio task inside a render */

void sloop_plat_locks_init(void)
{
    if (!s_audio_mx)
        s_audio_mx = xSemaphoreCreateMutex();
    if (!s_io_mx)
        s_io_mx = xSemaphoreCreateMutex();
}

void sloop_plat_audio_lock(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (me == s_rendering || me == s_holder)
        return;                                  /* inside the ISR, or already "cli" */
    xSemaphoreTake(s_audio_mx, portMAX_DELAY);
    s_holder = me;
}

void sloop_plat_audio_unlock(void)
{
    if (s_holder != xTaskGetCurrentTaskHandle())
        return;
    s_holder = NULL;
    xSemaphoreGive(s_audio_mx);
}

void sloop_plat_render_begin(void)
{
    xSemaphoreTake(s_audio_mx, portMAX_DELAY);
    s_rendering = xTaskGetCurrentTaskHandle();
}

void sloop_plat_render_end(void)
{
    s_rendering = NULL;
    xSemaphoreGive(s_audio_mx);
}

void sloop_plat_io_lock(void) { xSemaphoreTake(s_io_mx, portMAX_DELAY); }
void sloop_plat_io_unlock(void) { xSemaphoreGive(s_io_mx); }

/* -------------------------------------------------------------- storage --- */
static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_map_handle;
sloop_port_stats_t sloop_port_stats;

/* a staged backup: the image at 0, this trailer after it */
#define STAGE_MAGIC "SLOOPRST"
typedef struct {
    char magic[8];
    uint32_t size, crc;
} stage_trailer_t;

/* erase + write n bytes (a multiple of 4 KiB) sector by sector, each sector from internal RAM, as
 * SLOOP and OTA updates write (src is in PSRAM: the flash driver would bounce it in small pieces;
 * Espressif's QEMU takes ~100 s for 448 KiB that way, ~5 s like this) */
static esp_err_t part_put(const esp_partition_t *p, size_t off, const void *src, size_t n)
{
    uint8_t *sec = heap_caps_malloc(4096u, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = sec ? ESP_OK : ESP_ERR_NO_MEM;
    size_t i;
    for (i = 0; i < n && err == ESP_OK; i += 4096u) {
        memcpy(sec, (const uint8_t *)src + i, 4096u);
        err = esp_partition_erase_range(p, off + i, 4096u);
        if (err == ESP_OK)
            err = esp_partition_write(p, off + i, sec, 4096u);
    }
    free(sec);
    return err;
}

static const esp_partition_t *stage_part(void)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)SLOOP_STAGE_SUBTYPE, "stage");
    return p && p->size >= SLOOP_STORE_SIZE + 4096u ? p : NULL;
}

esp_err_t sloop_port_store_stage(const uint8_t *img, size_t n)
{
    const esp_partition_t *p = stage_part();
    stage_trailer_t t = {STAGE_MAGIC, (uint32_t)n, 0};
    esp_err_t err;
    if (!p)
        return ESP_ERR_NOT_FOUND;
    int64_t t0 = esp_timer_get_time(), t1, t2;
    if (n != SLOOP_STORE_SIZE || !sloop_store_check(img, n, NULL))
        return ESP_ERR_INVALID_ARG;
    t1 = esp_timer_get_time();
    t.crc = esp_rom_crc32_le(0, img, (uint32_t)n);
    t2 = esp_timer_get_time();
    err = esp_partition_erase_range(p, SLOOP_STORE_SIZE, 4096u);           /* no trailer while the image changes */
    if (err == ESP_OK)
        err = part_put(p, 0, img, n);
    if (err == ESP_OK)
        err = esp_partition_write(p, SLOOP_STORE_SIZE, &t, sizeof t);   /* the trailer last: the commit */
    ESP_LOGI(TAG, "store: a backup staged for the next boot (%s; check %d ms, crc %d ms, write %d ms)",
             esp_err_to_name(err), (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000),
             (int)((esp_timer_get_time() - t2) / 1000));
    return err;
}

/* (boot, before SLOOP) a staged backup, checked again, replaces the store; the stage is cleared */
static void store_apply_staged(void)
{
    const esp_partition_t *p = stage_part();
    stage_trailer_t t;
    uint8_t *img;
    sloop_store_info_t info;
    esp_err_t err;
    if (!p || esp_partition_read(p, SLOOP_STORE_SIZE, &t, sizeof t) != ESP_OK ||
        memcmp(t.magic, STAGE_MAGIC, sizeof t.magic))
        return;
    img = heap_caps_malloc(SLOOP_STORE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) {
        ESP_LOGE(TAG, "store: no memory to restore the staged backup (kept for the next boot)");
        return;
    }
    err = esp_partition_read(p, 0, img, SLOOP_STORE_SIZE);
    if (err != ESP_OK || t.size != SLOOP_STORE_SIZE || esp_rom_crc32_le(0, img, SLOOP_STORE_SIZE) != t.crc ||
        !sloop_store_check(img, SLOOP_STORE_SIZE, &info)) {
        ESP_LOGE(TAG, "store: the staged backup is damaged: not restored");
    } else {
        err = part_put(s_part, 0, img, SLOOP_STORE_SIZE);   /* (cut off: the trailer stays, the next boot redoes it) */
        if (err == ESP_OK)
            ESP_LOGI(TAG, "store: restored from a backup (projects %u, autosave %u, preset banks %u, samples %u)",
                     info.projects, info.autosave, info.preset_banks, info.samples);
        else
            ESP_LOGE(TAG, "store: restoring the backup failed (%s)", esp_err_to_name(err));
    }
    free(img);
    esp_partition_erase_range(p, SLOOP_STORE_SIZE, 4096u);   /* once: a failed restore is not retried */
}

esp_err_t sloop_port_store_init(void)
{
    esp_err_t err;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)SLOOP_PART_SUBTYPE, "sloop");
    if (!s_part) {
        ESP_LOGE(TAG, "no \"sloop\" data partition (subtype 0x%02x): settings and projects stay in RAM", SLOOP_PART_SUBTYPE);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_part->size < SLOOP_STORE_SIZE) {
        ESP_LOGE(TAG, "\"sloop\" partition is %u bytes, SLOOP needs %u", (unsigned)s_part->size, (unsigned)SLOOP_STORE_SIZE);
        s_part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    store_apply_staged();
    err = esp_partition_mmap(s_part, 0, SLOOP_STORE_SIZE, ESP_PARTITION_MMAP_DATA, (const void **)&s_map, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmap of the store failed (%s): user sample slots unavailable", esp_err_to_name(err));
        s_map = NULL;
    }
    ESP_LOGI(TAG, "store: partition \"sloop\" at 0x%06x, %u KiB (SLOOP flash 0x%05x..0x%05x)",
             (unsigned)s_part->address, (unsigned)(s_part->size / 1024u), (unsigned)SLOOP_STORE_LO, (unsigned)SLOOP_STORE_HI);
    return ESP_OK;
}

static int store_in(uint32_t off, uint32_t n)
{
    return s_part && off >= SLOOP_STORE_LO && off <= SLOOP_STORE_HI && n <= SLOOP_STORE_HI - off;
}

int sloop_plat_store_ok(void) { return s_part != NULL; }

int sloop_plat_store_read(uint32_t off, void *dst, uint32_t n)
{
    esp_err_t err;
    if (!store_in(off, n))
        return -1;
    err = esp_partition_read(s_part, off - SLOOP_STORE_LO, dst, n);
    sloop_port_stats.store_reads++;
    if (err != ESP_OK) {
        sloop_port_stats.store_errors++;
        return -2;
    }
    return 0;
}

int sloop_plat_store_erase(uint32_t off, uint32_t n)
{
    esp_err_t err;
    if (!store_in(off, n) || (off & 0xFFFu) || (n & 0xFFFu))
        return -1;
    err = esp_partition_erase_range(s_part, off - SLOOP_STORE_LO, n);
    sloop_port_stats.store_erases++;
    if (err != ESP_OK) {
        sloop_port_stats.store_errors++;
        ESP_LOGW(TAG, "erase 0x%05x: %s", (unsigned)off, esp_err_to_name(err));
        return -2;
    }
    return 0;
}

int sloop_plat_store_write(uint32_t off, const void *src, uint32_t n)
{
    esp_err_t err;
    if (!store_in(off, n))
        return -1;
    err = esp_partition_write(s_part, off - SLOOP_STORE_LO, src, n);
    sloop_port_stats.store_writes++;
    if (err != ESP_OK) {
        sloop_port_stats.store_errors++;
        ESP_LOGW(TAG, "write 0x%05x: %s", (unsigned)off, esp_err_to_name(err));
        return -2;
    }
    return 0;
}

const uint8_t *sloop_plat_store_ptr(uint32_t off)
{
    return (s_map && store_in(off, 1)) ? s_map + (off - SLOOP_STORE_LO) : NULL;
}

/* --------------------------------------------------------------- system --- */
void sloop_plat_reboot(void)
{
    ESP_LOGW(TAG, "SLOOP asked for a reset");
    esp_restart();
}

void sloop_plat_log(const char *msg) { ESP_LOGI("sloop", "%s", msg); }
