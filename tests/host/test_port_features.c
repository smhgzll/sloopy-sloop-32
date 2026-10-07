/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: SLOOP's features driven only through the virtual FM-1 panel, the way
 * the manual (upstream SLOOP.md) describes them, on the deterministic clock:
 *   the drum track (ALGORITHM), kits (PRESETS), a free take (REC, play, REC on the "1"), the loop
 *   playing by itself, undo / redo (EDIT + OCT- / OCT+), punch-in FX (FX + key), mute (GLO + key),
 *   tempo (SELECT), clearing a track (hold REC), the HOME menu, and a power cycle: the autosaved
 *   project comes back from the store file in a new process.
 * Screens at the main steps are hashed against tests/host/golden_screens.txt (GOLDEN_UPDATE=1
 * rewrites it) and written to OUTDIR as PPM for review.
 *   test_port_features [OUTDIR]                                                                   */
#include <sys/wait.h>
#include <unistd.h>

#include "sim.h"

static const char *outdir = "build-host/out";
static const unsigned WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
#define W(n) WHITE[(n) - 1]                    /* white key n (1..16) -> key 0..26 */

/* ---- screens against the golden file */
static FILE *golden_out;
static char golden_in[64][96];
static int ngolden;

static void golden_load(void)
{
    FILE *f;
    char line[96];
    if (getenv("GOLDEN_UPDATE") && (f = fopen(SLOOP_TESTS_DIR "/golden_screens.txt", "w"))) {
        fprintf(f, "# port screens (test_port_features): name FNV-1a of the 240x240 RGB565 screen\n");
        fclose(f);
        return;
    }
    f = fopen(SLOOP_TESTS_DIR "/golden_screens.txt", "r");
    if (!f)
        return;
    while (ngolden < 64 && fgets(line, sizeof line, f))
        if (line[0] != '#' && line[0] != '\n')
            snprintf(golden_in[ngolden++], sizeof golden_in[0], "%s", line);
    fclose(f);
}

static void screen(const char *name)
{
    char path[512], want[96] = "";
    uint64_t h = sim_screen_hash();
    int i;
    snprintf(path, sizeof path, "%s/feat-%s.ppm", outdir, name);
    sim_ppm(path);
    for (i = 0; i < ngolden; i++) {
        char n[64];
        unsigned long long v;
        if (sscanf(golden_in[i], "%63s %llx", n, &v) == 2 && !strcmp(n, name))
            snprintf(want, sizeof want, "%016llx", v);
    }
    if (getenv("GOLDEN_UPDATE")) {
        if (!golden_out)
            golden_out = fopen(SLOOP_TESTS_DIR "/golden_screens.txt", "a");
        if (golden_out) {
            fprintf(golden_out, "%s %016llx\n", name, (unsigned long long)h);
            fflush(golden_out);
        }
        return;
    }
    {
        char got[32], what[128];
        snprintf(got, sizeof got, "%016llx", (unsigned long long)h);
        snprintf(what, sizeof what, "screen %-12s matches the golden hash%s", name, want[0] ? "" : " (missing)");
        sim_check(want[0] && !strcmp(want, got), what);
    }
}

/* ---- audio over a stretch of time */
static int32_t peak_over(uint32_t ms)
{
    sim.peak = 0;
    sim_run_ms(ms);
    return sim.peak;
}

static void hold_tap(unsigned hold_btn, unsigned tap_btn)
{
    sloop_post_button(hold_btn, 1);
    sim_run_ms(250);                           /* (the layer opens after 0.14 s) */
    sim_tap(tap_btn, 60, 60);
    sloop_post_button(hold_btn, 0);
    sim_run_ms(200);
}

static void hold_key(unsigned hold_btn, unsigned key, uint32_t key_ms)
{
    sloop_post_button(hold_btn, 1);
    sim_run_ms(250);
    sim_key(key, key_ms, 60);
    sloop_post_button(hold_btn, 0);
    sim_run_ms(200);
}

/* turn an encoder detent by detent, as slowly as a hand does it on purpose (no acceleration) */
static void turn(unsigned enc, int detents)
{
    while (detents) {
        int d = detents > 0 ? 1 : -1;
        sloop_post_encoder(enc, d);
        detents -= d;
        sim_run_ms(100);
    }
}

static sloop_status_t status(void)
{
    sloop_status_t st;
    sloop_status_get(&st);
    return st;
}

/* the "sixty seconds to a beat" free take on the drum track: kicks on the beat, a snare on 2 and
 * 4, two bars at 120 BPM, REC on the "1" after them */
static void free_take(void)
{
    uint32_t beat;
    sim_tap(SLOOP_BTN_REC, 60, 300);           /* rec ready (empty project: a free take) */
    for (beat = 0; beat < 8u; beat++) {
        sloop_post_key(W(1), 1);               /* kick */
        if (beat % 2u)
            sloop_post_key(W(3), 1);           /* snare */
        sim_run_ms(60);
        sloop_post_key(W(1), 0);
        sloop_post_key(W(3), 0);
        sim_run_ms(440);
    }
    sim_tap(SLOOP_BTN_REC, 60, 0);             /* on the "1" after the last bar */
}

static int session_a(const char *store, uint16_t *bpm_out)
{
    sloop_status_t st;
    int32_t p;
    uint32_t r, e, w;

    sim_boot(store);
    sim_run_ms(1500);
    screen("tracks");

    /* ALGORITHM: the drum track */
    turn(SLOOP_ENC_ALGO, 3);
    sim_run_ms(200);
    sim_check(status().sel_track == 3, "ALGORITHM, 3 detents: track 4 (drums) selected");
    screen("drums-sel");
    p = peak_over(10);
    sim_key(W(1), 80, 600);
    sim_check(sim.peak > 20000, "drums: the F3 key plays the kick");
    (void)p;
    turn(SLOOP_ENC_PRESETS, 1);
    sim_run_ms(300);
    screen("kit-next");

    /* the free take */
    free_take();
    sim_run_ms(300);
    st = status();
    sim_check(st.playing == 1, "free take: REC on the \"1\" closes the loop, it plays at once");
    sim_check(st.bpm >= 116 && st.bpm <= 124, "free take: the tempo follows the playing (~120 BPM)");
    printf("    (tempo after the take: %d BPM)\n", st.bpm);
    *bpm_out = (uint16_t)st.bpm;
    sim_run_ms(1000);
    sim_check(peak_over(2000) > 20000, "loop: the recorded beat plays by itself");
    screen("playing");

    /* undo / redo */
    hold_tap(SLOOP_BTN_EDIT, SLOOP_BTN_OCTDN);
    sim_run_ms(1500);
    sim_check(peak_over(2000) < 3000, "EDIT + OCT-: undo empties the drum track (silence)");
    hold_tap(SLOOP_BTN_EDIT, SLOOP_BTN_OCTUP);
    sim_run_ms(500);
    sim_check(peak_over(2000) > 20000, "EDIT + OCT+: redo brings the beat back");

    /* punch-in FX: the mix changes while FX + a key are held */
    {
        uint64_t h0, h1;
        sim_run_ms(1000);
        sim.hash = 1;
        sim_run_ms(500);
        h0 = sim.hash;
        sloop_post_button(SLOOP_BTN_FX, 1);
        sim_run_ms(250);
        screen("fx-layer");
        sloop_post_key(W(5), 1);               /* stutter */
        sim.hash = 1;
        sim_run_ms(500);
        h1 = sim.hash;
        sloop_post_key(W(5), 0);
        sloop_post_button(SLOOP_BTN_FX, 0);
        sim_run_ms(300);
        sim_check(h0 != h1, "FX + key 5: a punch-in effect changes the mix");
    }

    /* GLO + key 4: the drum track muted */
    hold_key(SLOOP_BTN_GLO, W(4), 80);
    sim_run_ms(500);
    sim_check(peak_over(2000) < 3000, "GLO + key 4: the drums are muted");
    hold_key(SLOOP_BTN_GLO, W(4), 80);
    sim_run_ms(300);
    sim_check(peak_over(2000) > 20000, "GLO + key 4 again: unmuted");

    /* SELECT: tempo */
    {
        int16_t b0 = status().bpm;
        turn(SLOOP_ENC_SELECT, 5);
        sim_check(status().bpm == b0 + 5, "SELECT, 5 slow detents: tempo +5 BPM");
        turn(SLOOP_ENC_SELECT, -5);
        sim_check(status().bpm == b0, "SELECT, 5 back: the tempo as before");
        sim_run_ms(200);
        sloop_post_encoder(SLOOP_ENC_SELECT, 5);       /* one message of 5: a fast flick */
        sim_run_ms(300);
        sim_check(status().bpm > b0 + 5, "SELECT, a fast 5-detent turn: SLOOP accelerates it");
        printf("    (fast turn: %d -> %d BPM)\n", b0, status().bpm);
        sim_run_ms(200);
        turn(SLOOP_ENC_SELECT, -(status().bpm - b0));
    }

    /* stop, let the autosave write the project, then hold REC to clear the track and check that
     * the cleared state is what stays (the store keeps the last autosave until the next one) */
    sim_tap(SLOOP_BTN_PLAY, 60, 500);
    sim_check(status().playing == 0, "PLAY: stopped");
    host_store_stats(&r, &e, &w);
    sim_run_ms(30000);                         /* (2.5 s idle, 20 s after the last autosave) */
    {
        uint32_t r2, e2, w2;
        host_store_stats(&r2, &e2, &w2);
        sim_check(w2 > w && e2 > e, "autosave: the project is written to the store when idle");
    }

    /* HOME held: the menu */
    sloop_post_button(SLOOP_BTN_HOME, 1);
    sim_run_ms(1200);
    screen("home-menu");
    sloop_post_button(SLOOP_BTN_HOME, 0);
    sim_run_ms(300);
    sim_tap(SLOOP_BTN_HOME, 60, 300);
    return 0;
}

static int session_b(const char *store, uint16_t bpm_a)
{
    sim_boot(store);
    sim_run_ms(1500);
    sim_check(status().bpm == bpm_a, "power cycle: the tempo comes back from the store");
    sim_tap(SLOOP_BTN_PLAY, 60, 1000);
    sim_check(peak_over(2000) > 20000, "power cycle: the recorded beat comes back and plays");

    /* hold REC: the selected track is cleared (a ring fills for ~2 s) */
    turn(SLOOP_ENC_ALGO, 3);
    sim_check(status().sel_track == 3, "ALGORITHM: the drum track again");
    sloop_post_button(SLOOP_BTN_REC, 1);
    sim_run_ms(2600);
    screen("rec-held");
    sloop_post_button(SLOOP_BTN_REC, 0);
    sim_run_ms(1500);
    sim_check(peak_over(2000) < 3000, "hold REC ~2.5 s: the drum track is cleared");

    /* MIDI in (USB-MIDI packets, as the panel's "midi in" or a USB host sends them): a note plays */
    sim_tap(SLOOP_BTN_PLAY, 60, 1500);
    sloop_post_midi(0x09u | 0x90u << 8 | 60u << 16 | 100u << 24);
    sim.peak = 0;
    sim_run_ms(500);
    sloop_post_midi(0x08u | 0x80u << 8 | 60u << 16);
    sim_check(sim.peak > 3000, "MIDI in: a note on channel 1 is heard");
    sim_run_ms(1500);
    sim_check(peak_over(1000) < 3000, "MIDI in: the note off ends it (no hanging note)");
    return 0;
}

/* run fn in a child process (a power-on), passing a value back */
static int child(int (*fn)(const char *, uint16_t *), int (*fn_b)(const char *, uint16_t), const char *store,
                 uint16_t *io)
{
    int fd[2], st = 0;
    pid_t pid;
    if (pipe(fd))
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        uint16_t v = io ? *io : 0;
        if (fn)
            fn(store, &v);
        else
            fn_b(store, v);
        if (golden_out)
            fclose(golden_out);
        host_store_close();
        if (write(fd[1], &v, sizeof v) != sizeof v)
            _exit(2);
        fflush(stdout);
        _exit(sim_fails ? 1 : 0);
    }
    close(fd[1]);
    if (io && read(fd[0], io, sizeof *io) != sizeof *io)
        *io = 0;
    close(fd[0]);
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

int main(int argc, char **argv)
{
    char store[512];
    uint16_t bpm = 0;
    int rc_a, rc_b;
    if (argc > 1)
        outdir = argv[1];
    golden_load();
    snprintf(store, sizeof store, "%s/features-store.bin", outdir);
    unlink(store);
    printf("port features (session 1: a beat from scratch):\n");
    rc_a = child(session_a, NULL, store, &bpm);
    printf("port features (session 2: after a power cycle, same store):\n");
    rc_b = child(NULL, session_b, store, &bpm);
    printf("port features: %s\n", rc_a || rc_b ? "FAIL" : "PASS");
    return rc_a || rc_b;
}
