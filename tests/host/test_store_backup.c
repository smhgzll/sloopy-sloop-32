/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: SLOOP's store as a backup file (sloop_store_check, the device page's
 * backup / restore), on the deterministic clock, each power-on a new process:
 *   1. a fresh store: the tempo is changed with SELECT, the autosave writes the project
 *   2. that store file, taken as a backup, checks as a SLOOP store (the autosave: SLOOP writes its
 *      settings object only once they change); a damaged copy, a blank or short image, other data
 *      do not
 *   3. the backup restored onto another, fresh store (another board): the tempo comes back
 *   4. a fresh store without it: the default tempo (the control)
 *   test_store_backup [OUTDIR]                                                                    */
#include <sys/wait.h>
#include <unistd.h>

#include "sim.h"
#include "sloop_platform.h"

static void turn(unsigned enc, int detents)
{
    while (detents) {
        int d = detents > 0 ? 1 : -1;
        sloop_post_encoder(enc, d);
        detents -= d;
        sim_run_ms(100);
    }
}

static int16_t bpm_now(void)
{
    sloop_status_t st;
    sloop_status_get(&st);
    return st.bpm;
}

/* a power-on in a child process: boots on store, runs fn, returns its value */
static int power_on(const char *store, int16_t (*fn)(void), int16_t *out)
{
    int fd[2], st = 0;
    pid_t pid;
    if (pipe(fd))
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        int16_t v;
        sim_boot(store);
        sim_run_ms(1500);
        v = fn();
        host_store_close();
        if (write(fd[1], &v, sizeof v) != sizeof v)
            _exit(2);
        fflush(stdout);
        _exit(sim_fails ? 1 : 0);
    }
    close(fd[1]);
    if (read(fd[0], out, sizeof *out) != sizeof *out)
        *out = 0;
    close(fd[0]);
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

static int16_t change_tempo(void)
{
    uint32_t w0, w1, r, e;
    int16_t b0 = bpm_now();
    turn(SLOOP_ENC_SELECT, 7);
    sim_check(bpm_now() == b0 + 7, "SELECT: tempo +7 BPM");
    host_store_stats(&r, &e, &w0);
    sim_run_ms(30000);                         /* (2.5 s idle, 20 s after boot: the autosave) */
    host_store_stats(&r, &e, &w1);
    sim_check(w1 > w0, "the autosave writes the project");
    return bpm_now();
}

static int16_t read_tempo(void) { return bpm_now(); }

static int load(const char *path, uint8_t *img, size_t cap, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *n = fread(img, 1, cap, f);
    fclose(f);
    return 0;
}

static int save(const char *path, const uint8_t *img, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img, 1, n, f) != n)
        return -1;
    return fclose(f);
}

/* the sector of the autosave's copy that holds the commit record (storage.c: copy A 0x9F000,
 * copy B 0xFE000), whichever is valid */
static uint32_t autosave_sector(const uint8_t *img)
{
    static const uint32_t s[2] = {0x9F000u - SLOOP_STORE_LO, 0xFE000u - SLOOP_STORE_LO};
    int i;
    for (i = 0; i < 2; i++)
        if (!memcmp(img + s[i], "FELU", 4))
            return s[i];
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t backup[SLOOP_STORE_BYTES + 16], img[SLOOP_STORE_BYTES];
    const char *outdir = argc > 1 ? argv[1] : "build-host/out";
    char a[512], b[512], c[512];
    sloop_store_info_t info;
    size_t n = 0;
    int16_t set = 0, back = 0, dflt = 0;
    int rc = 0;
    uint32_t sec;

    snprintf(a, sizeof a, "%s/backup-a.bin", outdir);
    snprintf(b, sizeof b, "%s/backup-b.bin", outdir);
    snprintf(c, sizeof c, "%s/backup-c.bin", outdir);
    unlink(a);
    unlink(b);
    unlink(c);
    printf("store backup (session 1: a fresh store, the tempo changed, the autosave):\n");
    rc |= power_on(a, change_tempo, &set);

    printf("store backup (the store file as a backup):\n");
    sim_check(load(a, backup, sizeof backup, &n) == 0 && n == SLOOP_STORE_BYTES,
              "the backup is SLOOP's whole store window (448 KiB)");
    sim_check(sloop_store_check(backup, n, &info) == 1 && info.autosave,
              "it checks as a SLOOP store: the working project");
    printf("    (settings %u, projects %u, autosave %u, preset banks %u, sample slots %u)\n", info.settings,
           info.projects, info.autosave, info.preset_banks, info.samples);
    sim_check(!sloop_store_check(backup, n - 4096u, &info), "a short image is not a store");
    memset(img, 0xFF, sizeof img);
    sim_check(!sloop_store_check(img, sizeof img, &info), "a blank (erased) image is not a store");
    for (n = 0; n < sizeof img; n++)
        img[n] = (uint8_t)(n * 2654435761u >> 13);
    sim_check(!sloop_store_check(img, sizeof img, &info), "other data is not a store");
    memcpy(img, backup, sizeof img);
    sec = autosave_sector(img);
    img[sec + 256u + 10u] ^= 0x5A;            /* a payload byte of the working project */
    sim_check(sec && !sloop_store_check(img, sizeof img, &info) && !info.autosave,
              "a damaged copy does not count (its payload CRC): nothing valid left");
    memcpy(img, backup, sizeof img);
    img[sec + 12u] ^= 0x01;                    /* its header (len): the header CRC */
    sim_check(!sloop_store_check(img, sizeof img, &info) && !info.autosave, "a damaged header does not count");

    printf("store backup (session 2: restored onto another, fresh store):\n");
    sim_check(save(b, backup, SLOOP_STORE_BYTES) == 0, "the backup written to the other store");
    rc |= power_on(b, read_tempo, &back);
    sim_check(back == set, "the tempo comes back from the restored store");
    printf("store backup (session 3: a fresh store, no backup):\n");
    rc |= power_on(c, read_tempo, &dflt);
    sim_check(dflt != set, "the control: a fresh store has the default tempo");
    printf("    (tempo set %d, restored %d, default %d)\n", set, back, dflt);
    printf("store backup: %s\n", rc || sim_fails ? "FAIL" : "PASS");
    return rc || sim_fails;
}
