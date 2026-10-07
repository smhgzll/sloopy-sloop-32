/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: the SLOOP core boots on the virtual FM-1 and runs as on the device.
 * Boot (logo, then the TRACKS screen), the transport from the virtual PLAY button, notes from the
 * virtual keys reaching the audio, LEDs, and a deterministic audio hash across two runs.
 *   test_port_boot [OUTDIR]   (screens and audio for review go to OUTDIR, default build-host/out) */
#include <sys/wait.h>
#include <unistd.h>

#include "sim.h"

static uint64_t scenario(const char *outdir, int write)
{
    char path[512];
    sloop_status_t st;
    uint8_t led[SLOOP_LED_COUNT];
    uint32_t frames0;

    sim_boot(NULL);
    if (write) {
        snprintf(path, sizeof path, "%s/boot.wav", outdir);
        sim_wav_open(path);
    }
    sim_run_ms(300);
    if (write) {
        snprintf(path, sizeof path, "%s/screen-splash.ppm", outdir);
        sim_ppm(path);
        sim_check(sim_screen_lit() > 2000u, "boot: the SLOOP logo is on the screen");
    }
    sim_run_ms(1200);
    frames0 = sloop_display_frames();
    sloop_status_get(&st);
    if (write) {
        snprintf(path, sizeof path, "%s/screen-tracks.ppm", outdir);
        sim_ppm(path);
        sim_check(st.ui_frames > 30u, "boot: the main loop runs (UI frames)");
        printf("  (screens published so far: %u)\n", frames0);
        sim_check(frames0 >= 2u, "boot: screens are published (logo, then TRACKS)");
        sim_check(!st.playing && st.bpm > 0, "boot: stopped, a tempo is set");
        sim_check(sim.frames >= SLOOP_FS, "boot: audio rendered on time (>= 1 s in 1.5 s)");
    }

    /* the keys of track 1 (a synth part) make sound */
    sim_key(12, 400, 400);                         /* C4 */
    if (write) {
        sim_check(sim.peak > 1000, "keys: a key held on track 1 is heard");
        sim_check(sim.peak < (1 << 23), "keys: the output stays inside 24 bits");
    }

    /* PLAY starts the transport, PLAY again stops it */
    sim_tap(SLOOP_BTN_PLAY, 60, 400);
    sloop_status_get(&st);
    if (write)
        sim_check(st.playing == 1, "transport: PLAY tapped -> playing");
    sloop_leds_get(led);
    {
        uint32_t i, lit = 0;
        for (i = 0; i < 600; i += 10) {            /* PLAY flashes on the beat */
            sim_run_ms(10);
            sloop_leds_get(led);
            lit += led[SLOOP_BTN_PLAY] == 2;
        }
        if (write)
            sim_check(lit > 0 && lit < 60, "LEDs: PLAY flashes while playing");
    }
    sim_tap(SLOOP_BTN_PLAY, 60, 400);
    sloop_status_get(&st);
    if (write) {
        sim_check(st.playing == 0, "transport: PLAY tapped again -> stopped");
        snprintf(path, sizeof path, "%s/screen-after.ppm", outdir);
        sim_ppm(path);
    }
    sim_run_ms(1000);
    if (write)
        sim_wav_close();
    return sim.hash;
}

/* a power-on in a child process (the core's state is static): its audio hash */
static uint64_t scenario_fresh(void)
{
    int fd[2];
    uint64_t h = 0;
    pid_t pid;
    if (pipe(fd))
        return 0;
    pid = fork();
    if (pid == 0) {
        h = scenario(NULL, 0);
        if (write(fd[1], &h, sizeof h) != sizeof h)
            _exit(1);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], &h, sizeof h) != sizeof h)
        h = 0;
    close(fd[0]);
    waitpid(pid, NULL, 0);
    return h;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : "build-host/out";
    uint64_t h1, h2;
    printf("port boot: %s (upstream %s)\n", sloop_version(), sloop_upstream_commit());
    fflush(stdout);
    h2 = scenario_fresh();
    h1 = scenario(outdir, 1);
    printf("  audio hash %016llx, %llu frames, peak %d\n", (unsigned long long)h1,
           (unsigned long long)sim.frames, sim.peak);
    sim_check(h1 == h2, "determinism: the same input gives the same audio (two power-ons)");
    printf("port boot: %s\n", sim_fails ? "FAIL" : "PASS");
    return sim_fails ? 1 : 0;
}
