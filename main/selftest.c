/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the firmware's boot self-test (CONFIG_SLOOPY_SELFTEST, on in the QEMU profile).
 *
 * It drives SLOOP through the same mailboxes the web panel uses and checks what QEMU can check:
 * FreeRTOS tasks, PSRAM, the flash store, the core's behaviour. It cannot check the I2S / DAC
 * timing or Wi-Fi (not emulated): those are hardware tests (docs/HARDWARE_TESTS.md).
 *
 * Phase 1 (fresh flash):
 *   - the deterministic render (sloop_selftest_render) before boot: its hash must equal the host's
 *   - boot, screens published, UI frames, PSRAM size and SLOOP's .pool placed there
 *   - PLAY starts / stops the transport, keys reach the audio, encoders change parameters
 *   - the autosave writes the working project into the "sloop" partition
 *   then saves the expected values in NVS and restarts the chip.
 * Phase 2 (after esp_restart):
 *   - the project comes back from flash (track level as changed in phase 1)
 *   - 30 s of random use of the panel (buttons, holds, keys, encoders, MASTER, MIDI) from core 0
 *     while SLOOP runs on core 1: no crash, heap intact, stacks with margin, audio kept coming,
 *     SLOOP still answers PLAY
 * Console markers: SLOOP_SELFTEST_RENDER, SLOOP_QEMU_PHASE1_DONE, SLOOP_QEMU_PASS / SLOOP_QEMU_FAIL.
 * After a pass, later boots of the same flash (KEEP_FLASH=1, a firmware update) skip the self-test
 * (SLOOP_QEMU_IDLE) and just run SLOOP and the web panel; a fresh flash image runs it again.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "audio_backend.h"
#include "selftest.h"
#include "sloop.h"
#include "sloop_port.h"

static const char *TAG = "selftest";
#define SELFTEST_RENDER_MS 4000u

static int s_fails, s_phase;
static int32_t s_level_set;

static void check(int ok, const char *what)
{
    ESP_LOGI(TAG, "%-60s %s", what, ok ? "ok" : "FAIL");
    if (!ok)
        s_fails++;
}

static int nvs_get(const char *key, int32_t *v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("selftest", NVS_READONLY, &h);
    if (err != ESP_OK)
        return 0;
    err = nvs_get_i32(h, key, v);
    nvs_close(h);
    return err == ESP_OK;
}

static void nvs_set(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open("selftest", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void selftest_before_boot(void)
{
    int32_t phase = 1;
    if (nvs_get("phase", &phase) && phase == 3) {
        s_phase = 0;                              /* passed before on this flash: idle */
        return;
    }
    if (phase == 2) {
        s_phase = 2;
        nvs_get("level", &s_level_set);
        return;
    }
    s_phase = 1;
    {
        int32_t peak = 0;
        int64_t t0 = esp_timer_get_time();
        uint64_t h = sloop_selftest_render(SELFTEST_RENDER_MS, &peak);
        int64_t us = esp_timer_get_time() - t0;
        printf("SLOOP_SELFTEST_RENDER ms=%u hash=%016" PRIx64 " peak=%" PRId32 " took_ms=%d\n", SELFTEST_RENDER_MS, h,
               peak, (int)(us / 1000));
    }
}

static int wait_for(int (*cond)(void), uint32_t timeout_ms)
{
    uint32_t t;
    for (t = 0; t < timeout_ms; t += 50) {
        if (cond())
            return 1;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return cond();
}

static int is_playing(void)
{
    sloop_status_t st;
    sloop_status_get(&st);
    return st.playing;
}
static int is_stopped(void) { return !is_playing(); }
static int is_booted(void) { return sloop_port_booted(); }
static int store_written(void)
{
    sloop_port_stats_t ps;
    sloop_port_get_stats(&ps);
    return ps.store_writes > 0;
}

static void tap(unsigned btn, uint32_t hold_ms)
{
    sloop_post_button(btn, 1);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    sloop_post_button(btn, 0);
}

static void report_stats(void)
{
    sloop_port_stats_t ps;
    audio_backend_stats_t as;
    sloop_status_t st;
    sloop_port_get_stats(&ps);
    audio_backend_get_stats(&as);
    sloop_status_get(&st);
    ESP_LOGI(TAG, "audio: %" PRIu64 " frames, %" PRIu32 " late, peak %" PRId32 ", render max %" PRIu32 " us, cpu %u/256",
             as.frames, as.late, as.peak, ps.render_max_us, st.cpu_q8);
    ESP_LOGI(TAG, "ui: %" PRIu32 " frames, %" PRIu32 " steps, %u screens; store r/e/w %" PRIu32 "/%" PRIu32 "/%" PRIu32
             " (%" PRIu32 " errors)",
             st.ui_frames, ps.ui_steps, (unsigned)sloop_display_frames(), ps.store_reads, ps.store_erases,
             ps.store_writes, ps.store_errors);
    ESP_LOGI(TAG, "stack free: audio %" PRIu32 " B, ui %" PRIu32 " B; heap internal %u, PSRAM %u, min internal %u",
             ps.audio_stack_free, ps.ui_stack_free, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static void phase1(void)
{
    sloop_status_t st;
    audio_backend_stats_t a0, a1;
    int lvl0 = 0, lvl1 = 0;
    uint32_t screens0;

    check(esp_psram_get_size() >= 16u * 1024u * 1024u, "PSRAM: 16 MB detected");
    check(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 15u * 1024u * 1024u, "PSRAM: heap available");
    vTaskDelay(pdMS_TO_TICKS(1500));                     /* the logo, then the TRACKS screen */
    sloop_status_get(&st);
    check(st.ui_frames > 10u, "UI task: SLOOP's main loop runs");
    check(sloop_display_frames() >= 2u, "display: screens published (logo, TRACKS)");

    tap(SLOOP_BTN_PLAY, 80);
    check(wait_for(is_playing, 3000), "transport: PLAY -> playing");
    audio_backend_get_stats(&a0);
    sloop_post_key(12, 1);                               /* C4 on track 1 */
    vTaskDelay(pdMS_TO_TICKS(600));
    sloop_post_key(12, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    audio_backend_get_stats(&a1);
    check(a1.frames > a0.frames, "audio task: halves rendered while playing");
    check(a1.peak > 1000 && a1.nonzero_frames > a0.nonzero_frames, "audio: a key on track 1 is heard");
    tap(SLOOP_BTN_PLAY, 80);
    check(wait_for(is_stopped, 3000), "transport: PLAY again -> stopped");

    sloop_param_get(0, "LVL", &lvl0);
    screens0 = sloop_display_frames();
    sloop_post_encoder(SLOOP_ENC_K2, -6);                /* TRACKS: KNOB 2 = level of the selected track */
    vTaskDelay(pdMS_TO_TICKS(300));
    sloop_param_get(0, "LVL", &lvl1);
    check(lvl1 != lvl0, "encoder: KNOB 2 changes track 1's level");
    check(sloop_display_frames() > screens0, "display: the change is redrawn");
    s_level_set = lvl1;

    ESP_LOGI(TAG, "waiting for SLOOP's autosave (2.5 s idle, >= 20 s after boot)");
    check(wait_for(store_written, 40000), "store: the autosave writes the project to the \"sloop\" partition");
    vTaskDelay(pdMS_TO_TICKS(500));
    report_stats();
    check(heap_caps_check_integrity_all(true), "heap: integrity");
    if (s_fails) {
        printf("SLOOP_QEMU_FAIL phase=1 fails=%d\n", s_fails);
        return;
    }
    nvs_set("phase", 2);
    nvs_set("level", s_level_set);
    printf("SLOOP_QEMU_PHASE1_DONE level=%" PRId32 "\n", s_level_set);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static uint32_t s_rnd = 0x2545F491u;
static uint32_t rnd(uint32_t n)
{
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd % n;
}

static void fuzz(uint32_t ms)
{
    uint32_t held_btn = 0, held_key = 0, i, events = 0;
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    audio_backend_stats_t a0, a1;
    sloop_port_stats_t ps;
    audio_backend_get_stats(&a0);
    while (esp_timer_get_time() < end) {
        uint32_t what = rnd(100);
        if (what < 30) {
            uint32_t k = rnd(SLOOP_NKEYS);
            int down = !((held_key >> k) & 1u);
            if (!down || __builtin_popcount(held_key) < 6) {
                sloop_post_key(k, down);
                held_key ^= 1u << k;
            }
        } else if (what < 50) {
            /* (not HOME: its menu leads to HARDWARE CALIBRATION, which by design re-learns what each
             * physical button means; random presses there would remap the panel) */
            uint32_t b = rnd(SLOOP_BTN_COUNT);
            int down;
            if (b == SLOOP_BTN_HOME)
                b = SLOOP_BTN_FX;
            down = !((held_btn >> b) & 1u);
            if (!down || __builtin_popcount(held_btn) < 2) {
                sloop_post_button(b, down);
                held_btn ^= 1u << b;
            }
        } else if (what < 75) {
            int d = rnd(4) ? (rnd(2) ? 1 : -1) : (int)rnd(9) - 4;
            if (d)
                sloop_post_encoder(rnd(SLOOP_ENC_COUNT), d);
        } else if (what < 80) {
            sloop_post_pot(SLOOP_POT_MASTER, rnd(1024));
        } else if (what < 92) {
            uint32_t ch = rnd(16), note = 36u + rnd(48), on = rnd(2);
            sloop_post_midi((on ? 0x09u : 0x08u) | ((on ? 0x90u : 0x80u) | ch) << 8 | note << 16 | (on ? 90u : 0u) << 24);
        } else {
            tap(SLOOP_BTN_PLAY, 40);
        }
        events++;
        vTaskDelay(pdMS_TO_TICKS(5 + rnd(60)));
    }
    for (i = 0; i < SLOOP_NKEYS; i++)
        if ((held_key >> i) & 1u)
            sloop_post_key(i, 0);
    for (i = 0; i < SLOOP_BTN_COUNT; i++)
        if ((held_btn >> i) & 1u)
            sloop_post_button(i, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    audio_backend_get_stats(&a1);
    sloop_port_get_stats(&ps);
    ESP_LOGI(TAG, "random use: %u events, audio peak %ld, stack free audio %lu / ui %lu", (unsigned)events,
             (long)a1.peak, (unsigned long)ps.audio_stack_free, (unsigned long)ps.ui_stack_free);
    check(a1.frames > a0.frames + (uint64_t)ms * 44100u / 1000u / 4u, "random use: audio kept coming");
    check(a1.peak < (1 << 23), "random use: the output stayed inside 24 bits");
    check(ps.audio_stack_free > 1024 && ps.ui_stack_free > 1024, "random use: task stacks kept > 1 KB free");
    check(heap_caps_check_integrity_all(true), "random use: heap integrity");
}

static void phase2(void)
{
    int lvl = -1;
    vTaskDelay(pdMS_TO_TICKS(1500));
    sloop_param_get(0, "LVL", &lvl);
    ESP_LOGI(TAG, "after restart: track 1 level %d (set %" PRId32 " before)", lvl, s_level_set);
    check(lvl == s_level_set, "store: the project came back from flash after a restart");
    fuzz(30000);
    {   /* SLOOP still answers: PLAY toggles the transport */
        int was = is_playing();
        sloop_status_t st;
        sloop_status_get(&st);
        ESP_LOGI(TAG, "after random use: playing %u, rec %u, track %u, page %u, menu %u, bpm %d", st.playing,
                 st.recording, st.sel_track, st.page, st.menu, st.bpm);
        tap(SLOOP_BTN_PLAY, 80);
        vTaskDelay(pdMS_TO_TICKS(500));
        check(is_playing() != was, "random use: SLOOP still answers PLAY");
        if (is_playing())
            tap(SLOOP_BTN_PLAY, 80);
    }
    report_stats();
    nvs_set("phase", s_fails ? 1 : 3);
    if (s_fails)
        printf("SLOOP_QEMU_FAIL phase=2 fails=%d\n", s_fails);
    else
        printf("SLOOP_QEMU_PASS\n");
}

static void selftest_task(void *arg)
{
    (void)arg;
    if (!wait_for(is_booted, 20000)) {
        printf("SLOOP_QEMU_FAIL boot timeout\n");
        vTaskDelete(NULL);
    }
    if (s_phase == 2)
        phase2();
    else
        phase1();
    vTaskDelete(NULL);
}

void selftest_start(void)
{
    if (s_phase == 0) {
        printf("SLOOP_QEMU_IDLE: the self-test passed on this flash before (a fresh image runs it)\n");
        return;
    }
    xTaskCreatePinnedToCore(selftest_task, "selftest", 6144, NULL, 3, NULL, 0);
}
