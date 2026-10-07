/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host tests: a deterministic run of the SLOOP core on the virtual clock.
 * The UI context steps every millisecond, the audio context renders a half buffer whenever
 * 256 frames of time have passed (as the I2S DMA would ask), in one thread, so every run of
 * a test is identical. Audio is hashed (FNV-1a over the 24-bit samples) and can go to a WAV. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform_host.h"
#include "sloop.h"

static struct {
    uint64_t t_us;                   /* virtual time since sim_boot */
    uint64_t frames;                 /* audio frames rendered */
    uint64_t hash;
    int32_t peak;
    uint64_t nonzero;                /* frames with a non-zero sample */
    FILE *wav;
    uint32_t wav_frames;
} sim;

static int sim_fails;
static void sim_check(int ok, const char *what)
{
    printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        sim_fails++;
}

static void sim_wav_open(const char *path)
{
    static const uint8_t hdr[44] = {0};
    sim.wav = fopen(path, "wb");
    if (sim.wav)
        fwrite(hdr, 1, sizeof hdr, sim.wav);
    sim.wav_frames = 0;
}

static void sim_wav_close(void)
{
    uint32_t v;
    uint16_t s;
    if (!sim.wav)
        return;
    fseek(sim.wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, sim.wav); v = 36u + sim.wav_frames * 4u; fwrite(&v, 4, 1, sim.wav);
    fwrite("WAVEfmt ", 1, 8, sim.wav); v = 16; fwrite(&v, 4, 1, sim.wav);
    s = 1; fwrite(&s, 2, 1, sim.wav); s = 2; fwrite(&s, 2, 1, sim.wav);
    v = SLOOP_FS; fwrite(&v, 4, 1, sim.wav); v = SLOOP_FS * 4u; fwrite(&v, 4, 1, sim.wav);
    s = 4; fwrite(&s, 2, 1, sim.wav); s = 16; fwrite(&s, 2, 1, sim.wav);
    fwrite("data", 1, 4, sim.wav); v = sim.wav_frames * 4u; fwrite(&v, 4, 1, sim.wav);
    fclose(sim.wav);
    sim.wav = NULL;
}

static void sim_audio_half(void)
{
    const int32_t *o = sloop_audio_render();
    uint32_t i;
    for (i = 0; i < 2u * SLOOP_HALF_FRAMES; i++) {
        int32_t v = o[i];
        uint32_t u = (uint32_t)v & 0xFFFFFFu;
        sim.hash = (sim.hash ^ (u & 0xFFu)) * 0x100000001B3ull;
        sim.hash = (sim.hash ^ ((u >> 8) & 0xFFu)) * 0x100000001B3ull;
        sim.hash = (sim.hash ^ (u >> 16)) * 0x100000001B3ull;
        if (v < 0)
            v = -v;
        if (v > sim.peak)
            sim.peak = v;
    }
    for (i = 0; i < SLOOP_HALF_FRAMES; i++)
        if (o[2u * i] || o[2u * i + 1u])
            sim.nonzero++;
    if (sim.wav) {
        int16_t s[2u * SLOOP_HALF_FRAMES];
        for (i = 0; i < 2u * SLOOP_HALF_FRAMES; i++) {
            int32_t v = o[i] >> 8;                 /* 24 -> 16 bit */
            s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        fwrite(s, sizeof s, 1, sim.wav);
        sim.wav_frames += SLOOP_HALF_FRAMES;
    }
    sim.frames += SLOOP_HALF_FRAMES;
}

/* run ms milliseconds: 1 ms UI steps, audio halves on time */
static void sim_run_ms(uint32_t ms)
{
    while (ms--) {
        host_clock_advance_us(1000);
        sim.t_us += 1000;
        while (sim.frames * 1000000u / SLOOP_FS + SLOOP_HALF_FRAMES * 1000000u / SLOOP_FS <= sim.t_us)
            sim_audio_half();
        sloop_ui_step();
    }
}

static void sim_boot(const char *store_path)
{
    memset(&sim, 0, sizeof sim);
    sim.hash = 0xCBF29CE484222325ull;
    host_clock_virtual(1);
    if (host_store_open(store_path)) {
        fprintf(stderr, "sim: cannot open the store\n");
        exit(2);
    }
    sloop_boot();
}

/* the largest |sample| over the next ms milliseconds */
static int32_t sim_peak_over(uint32_t ms)
{
    sim.peak = 0;
    sim_run_ms(ms);
    return sim.peak;
}

/* a control tapped / held as a hand would: down, held ms, up, then settle ms */
static void sim_tap(unsigned btn, uint32_t hold_ms, uint32_t settle_ms)
{
    sloop_post_button(btn, 1);
    sim_run_ms(hold_ms);
    sloop_post_button(btn, 0);
    sim_run_ms(settle_ms);
}
static void sim_key(unsigned key, uint32_t hold_ms, uint32_t settle_ms)
{
    sloop_post_key(key, 1);
    sim_run_ms(hold_ms);
    sloop_post_key(key, 0);
    sim_run_ms(settle_ms);
}

/* the published screen (whole) to a binary PPM */
static int sim_ppm(const char *path)
{
    static uint8_t fb[SLOOP_LCD_BYTES];
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f)
        return -1;
    sloop_display_copy_full(fb);
    fprintf(f, "P6\n%u %u\n255\n", SLOOP_LCD_W, SLOOP_LCD_H);
    for (i = 0; i < SLOOP_LCD_W * SLOOP_LCD_H; i++) {
        uint32_t p = (uint32_t)fb[2u * i] << 8 | fb[2u * i + 1u];
        uint8_t rgb[3] = {(uint8_t)((p >> 11) * 255u / 31u), (uint8_t)(((p >> 5) & 63u) * 255u / 63u),
                          (uint8_t)((p & 31u) * 255u / 31u)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* FNV-1a of the published screen */
static uint64_t sim_screen_hash(void)
{
    static uint8_t fb[SLOOP_LCD_BYTES];
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t i;
    sloop_display_copy_full(fb);
    for (i = 0; i < SLOOP_LCD_BYTES; i++)
        h = (h ^ fb[i]) * 0x100000001B3ull;
    return h;
}

/* pixels of the published screen that are not black */
static uint32_t sim_screen_lit(void)
{
    static uint8_t fb[SLOOP_LCD_BYTES];
    uint32_t i, n = 0;
    sloop_display_copy_full(fb);
    for (i = 0; i < SLOOP_LCD_W * SLOOP_LCD_H; i++)
        n += fb[2u * i] || fb[2u * i + 1u];
    return n;
}
