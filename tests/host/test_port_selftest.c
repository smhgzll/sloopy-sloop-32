/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: the deterministic cross-target render (port/sloop_selftest.c).
 * Prints the hash the ESP32 firmware must print under QEMU (scripts/test-qemu.sh compares) and
 * writes it to OUTDIR/selftest.hash. */
#include "sim.h"

#define SELFTEST_MS 4000u

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : "build-host/out";
    char path[512];
    int32_t peak = 0;
    uint64_t h;
    FILE *f;
    host_clock_virtual(1);
    host_store_open(NULL);
    h = sloop_selftest_render(SELFTEST_MS, &peak);
    printf("SLOOP_SELFTEST_RENDER ms=%u hash=%016llx peak=%d\n", SELFTEST_MS, (unsigned long long)h, peak);
    sim_check(peak > 3000 && peak < 32768 * 4, "selftest render: the song is audible and bounded");
    snprintf(path, sizeof path, "%s/selftest.hash", outdir);
    if ((f = fopen(path, "w"))) {
        fprintf(f, "%016llx\n", (unsigned long long)h);
        fclose(f);
    }
    /* SLOOP still boots normally afterwards (the firmware's QEMU self-test does the same) */
    sloop_boot();
    sim_run_ms(1500);
    {
        sloop_status_t st;
        sloop_status_get(&st);
        sim_check(st.ui_frames > 30u && !st.playing, "selftest render: SLOOP boots normally after it");
    }
    printf("port selftest: %s\n", sim_fails ? "FAIL" : "PASS");
    return sim_fails ? 1 : 0;
}
