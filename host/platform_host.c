/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the SLOOP platform on Linux (host app and host tests).
 *
 *   clock     CLOCK_MONOTONIC, or a virtual clock that only moves when told (deterministic tests;
 *             sleeping then advances it)
 *   locks     pthread mutexes; the audio lock has cli / sti semantics (non-nesting, no-op inside
 *             the render)
 *   storage   SLOOP's flash window with NOR semantics (erase -> 0xFF, programming ANDs), in a
 *             file (persistent) or in memory
 */
#define _GNU_SOURCE
#include "platform_host.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sloop_platform.h"

/* ---------------------------------------------------------------- clock --- */
static int clk_virtual;
static uint64_t clk_virtual_us = 1000000u;       /* (not 0: "never" stays distinguishable) */
static pthread_mutex_t clk_mx = PTHREAD_MUTEX_INITIALIZER;

void host_clock_virtual(int on) { clk_virtual = on; }
void host_clock_advance_us(uint64_t us)
{
    pthread_mutex_lock(&clk_mx);
    clk_virtual_us += us;
    pthread_mutex_unlock(&clk_mx);
}

uint64_t sloop_plat_time_us(void)
{
    struct timespec ts;
    if (clk_virtual) {
        uint64_t t;
        pthread_mutex_lock(&clk_mx);
        t = clk_virtual_us;
        pthread_mutex_unlock(&clk_mx);
        return t;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void sloop_plat_sleep_us(uint32_t us)
{
    struct timespec ts;
    if (clk_virtual) {
        host_clock_advance_us(us);
        return;
    }
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000;
    while (nanosleep(&ts, &ts) && errno == EINTR)
        ;
}

/* ---------------------------------------------------------------- locks --- */
static pthread_mutex_t audio_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t io_mx = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_in_render, t_held;

void sloop_plat_audio_lock(void)
{
    if (t_in_render || t_held)
        return;
    pthread_mutex_lock(&audio_mx);
    t_held = 1;
}
void sloop_plat_audio_unlock(void)
{
    if (t_in_render || !t_held)
        return;
    t_held = 0;
    pthread_mutex_unlock(&audio_mx);
}
void sloop_plat_render_begin(void)
{
    pthread_mutex_lock(&audio_mx);
    t_in_render = 1;
}
void sloop_plat_render_end(void)
{
    t_in_render = 0;
    pthread_mutex_unlock(&audio_mx);
}
void sloop_plat_io_lock(void) { pthread_mutex_lock(&io_mx); }
void sloop_plat_io_unlock(void) { pthread_mutex_unlock(&io_mx); }

/* -------------------------------------------------------------- storage --- */
static uint8_t *store;                           /* SLOOP_STORE_SIZE bytes: offset - SLOOP_STORE_LO */
static uint32_t store_ops[3];                    /* reads, erases, writes */

int host_store_open(const char *path)
{
    if (store)
        host_store_close();
    if (!path) {
        store = mmap(NULL, SLOOP_STORE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (store == MAP_FAILED) {
            store = NULL;
            return -1;
        }
        memset(store, 0xFF, SLOOP_STORE_SIZE);   /* a new part: erased */
        return 0;
    }
    {
        int fd = open(path, O_RDWR | O_CREAT, 0644);
        struct stat st;
        int fresh;
        if (fd < 0) {
            fprintf(stderr, "store: %s: %s\n", path, strerror(errno));
            return -1;
        }
        fstat(fd, &st);
        fresh = st.st_size == 0;
        if (st.st_size != SLOOP_STORE_SIZE && ftruncate(fd, SLOOP_STORE_SIZE)) {
            close(fd);
            return -1;
        }
        store = mmap(NULL, SLOOP_STORE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (store == MAP_FAILED) {
            store = NULL;
            return -1;
        }
        if (fresh)
            memset(store, 0xFF, SLOOP_STORE_SIZE);
    }
    return 0;
}

void host_store_close(void)
{
    if (store) {
        msync(store, SLOOP_STORE_SIZE, MS_SYNC);
        munmap(store, SLOOP_STORE_SIZE);
    }
    store = NULL;
}

uint8_t *host_store_image(void) { return store; }
void host_store_stats(uint32_t *reads, uint32_t *erases, uint32_t *writes)
{
    *reads = store_ops[0];
    *erases = store_ops[1];
    *writes = store_ops[2];
}

static int store_in(uint32_t off, uint32_t n)
{
    return store && off >= SLOOP_STORE_LO && off <= SLOOP_STORE_HI && n <= SLOOP_STORE_HI - off;
}

int sloop_plat_store_ok(void) { return store != NULL; }

int sloop_plat_store_read(uint32_t off, void *dst, uint32_t n)
{
    if (!store_in(off, n))
        return -1;
    memcpy(dst, store + (off - SLOOP_STORE_LO), n);
    store_ops[0]++;
    return 0;
}

int sloop_plat_store_erase(uint32_t off, uint32_t n)
{
    if (!store_in(off, n) || (off & 0xFFFu) || (n & 0xFFFu))
        return -1;
    memset(store + (off - SLOOP_STORE_LO), 0xFF, n);
    store_ops[1]++;
    return 0;
}

int sloop_plat_store_write(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    uint8_t *d;
    const uint8_t *s = src;
    if (!store_in(off, n))
        return -1;
    d = store + (off - SLOOP_STORE_LO);
    for (i = 0; i < n; i++)
        d[i] &= s[i];                            /* NOR: programming only clears bits */
    store_ops[2]++;
    return 0;
}

const uint8_t *sloop_plat_store_ptr(uint32_t off)
{
    return store_in(off, 1) ? store + (off - SLOOP_STORE_LO) : NULL;
}

/* --------------------------------------------------------------- system --- */
static void (*reboot_hook)(void);
void host_set_reboot_hook(void (*fn)(void)) { reboot_hook = fn; }

void sloop_plat_reboot(void)
{
    fprintf(stderr, "sloop: reboot requested\n");
    if (reboot_hook)
        reboot_hook();
    exit(3);
}

void sloop_plat_log(const char *msg) { fprintf(stderr, "sloop: %s\n", msg); }
