/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: random use of the virtual panel for a long time (virtual clock).
 * Buttons tapped and held (layers, combinations), keys, encoder turns fast and slow, the MASTER pot,
 * MIDI notes, the transport started and stopped, in random order. Invariants:
 *   - the output stays inside 24 bits, never NaN-like garbage (bounded), audio keeps coming
 *   - the UI keeps running (frames) and the screen keeps being published
 *   - after everything is released and the transport stopped, the instrument falls silent: no
 *     hanging voice from any input path
 *   test_port_soak [OUTDIR] [MINUTES]   (SOAK_MIN=n also sets the minutes; default 3) */
#include "sim.h"

static uint32_t rnd_state = 0x2545F491u;
static uint32_t rnd(uint32_t n)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state % n;
}

int main(int argc, char **argv)
{
    const char *env = getenv("SOAK_MIN");
    uint32_t minutes = argc > 2 ? (uint32_t)atoi(argv[2]) : env ? (uint32_t)atoi(env) : 3u;
    uint32_t t, events = 0, frames0, held_btn = 0, held_key = 0, ms_total = minutes * 60000u;
    sloop_status_t st;
    int32_t peak_all = 0;
    if (getenv("SOAK_SEED"))
        rnd_state = (uint32_t)strtoul(getenv("SOAK_SEED"), NULL, 0) | 1u;
    printf("port soak: %u minutes of random panel use (virtual time)\n", minutes);
    sim_boot(NULL);
    sim_run_ms(1500);
    frames0 = sloop_display_frames();
    for (t = 0; t < ms_total; ) {
        uint32_t what = rnd(100), step = 5u + rnd(120);
        if (what < 30) {                                   /* a key down or up */
            uint32_t k = rnd(SLOOP_NKEYS);
            int down = !((held_key >> k) & 1u);
            if (!down || __builtin_popcount(held_key) < 6) {
                sloop_post_key(k, down);
                held_key ^= 1u << k;
            }
        } else if (what < 50) {                            /* a button down or up (holds: layers) */
            uint32_t b = rnd(SLOOP_BTN_COUNT);
            int down = !((held_btn >> b) & 1u);
            if (b == SLOOP_BTN_HOME)                       /* (not HOME: its menu leads to HARDWARE */
                b = SLOOP_BTN_FX;                          /* CALIBRATION, which re-learns the panel) */
            down = !((held_btn >> b) & 1u);
            if (!down || __builtin_popcount(held_btn) < 2) {
                sloop_post_button(b, down);
                held_btn ^= 1u << b;
            }
        } else if (what < 75) {                            /* an encoder: slow detents or a flick */
            uint32_t e = rnd(SLOOP_ENC_COUNT);
            int d = rnd(4) ? (rnd(2) ? 1 : -1) : (int)rnd(9) - 4;
            if (e == SLOOP_ENC_SELECT && d > 0 && rnd(2))  /* (keep the tempo in a sane band mostly) */
                d = -d;
            if (d)
                sloop_post_encoder(e, d);
        } else if (what < 80) {
            sloop_post_pot(SLOOP_POT_MASTER, rnd(1024));
        } else if (what < 90) {                            /* MIDI notes on random channels */
            uint32_t ch = rnd(16), note = 36u + rnd(48), on = rnd(2);
            sloop_post_midi((on ? 0x09u : 0x08u) | ((on ? 0x90u : 0x80u) | ch) << 8 | note << 16 | (on ? 90u : 0u) << 24);
        } else if (what < 93) {
            sim_tap(SLOOP_BTN_PLAY, 40, 0);                /* transport */
            t += 40;
        }
        events++;
        sim.peak = 0;
        sim_run_ms(step);
        t += step;
        if (sim.peak > peak_all)
            peak_all = sim.peak;
        if (sim.peak >= (1 << 23)) {
            sim_check(0, "output inside 24 bits");
            break;
        }
    }
    /* let everything go, stop, wait for the tails */
    {
        uint32_t i, ch, n;
        for (i = 0; i < SLOOP_NKEYS; i++)
            if ((held_key >> i) & 1u)
                sloop_post_key(i, 0);
        for (i = 0; i < SLOOP_BTN_COUNT; i++)
            if ((held_btn >> i) & 1u)
                sloop_post_button(i, 0);
        for (ch = 0; ch < 16u; ch++)
            for (n = 36; n < 84u; n++)
                sloop_post_midi(0x08u | (0x80u | ch) << 8 | n << 16);
        sim_run_ms(500);
        sim_tap(SLOOP_BTN_HOME, 40, 300);                  /* (out of any menu / page) */
        sloop_status_get(&st);
        if (st.playing)
            sim_tap(SLOOP_BTN_PLAY, 40, 300);
        sloop_status_get(&st);
        if (st.playing)
            sim_tap(SLOOP_BTN_PLAY, 40, 300);
    }
    sim_run_ms(8000);                                      /* reverb / delay tails */
    sloop_status_get(&st);
    printf("  (%u events, peak %d, %u screens published, tempo %d)\n", events, peak_all,
           sloop_display_frames() - frames0, st.bpm);
    sim_check(peak_all < (1 << 23), "random use: the output stays inside 24 bits");
    sim_check(sim.frames > (uint64_t)(ms_total / 1000u) * SLOOP_FS * 9u / 10u, "random use: audio kept coming");
    sim_check(sloop_display_frames() - frames0 > 100u, "random use: the screen kept being published");
    sim_check(!st.playing, "after: stopped");
    sim_check(st.ui_frames > ms_total / 20u, "after: the UI kept running");
    {   /* SLOOP still answers PLAY */
        sloop_status_t a, b;
        sloop_status_get(&a);
        sim_tap(SLOOP_BTN_PLAY, 60, 400);
        sloop_status_get(&b);
        if (b.playing == a.playing)
            printf("  (state: playing %u rec %u track %u page %u menu %u)\n", b.playing, b.recording, b.sel_track, b.page, b.menu);
        sim_check(b.playing != a.playing, "after: PLAY starts the transport");
        sim_tap(SLOOP_BTN_PLAY, 60, 400);
        sim_run_ms(6000);
    }
    sim_check(sim_peak_over(3000) < 600, "after: silent (no hanging voice)");
    printf("port soak: %s\n", sim_fails ? "FAIL" : "PASS");
    return sim_fails ? 1 : 0;
}
