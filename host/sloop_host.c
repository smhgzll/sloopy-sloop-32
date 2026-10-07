/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host app: the SLOOP core (the same sources as the ESP32 firmware) on Linux,
 * with the browser FM-1 panel served over HTTP + WebSocket and the audio on the sound card.
 *
 *   sloop_host [--port N] [--bind ADDR] [--web DIR] [--store FILE] [--audio alsa|null]
 *              [--device ALSA_DEV] [--wav FILE] [--seconds S]
 *
 * Threads: UI (sloop_boot + sloop_ui_step, the FM-1 main loop), audio (sloop_audio_render ->
 * ALSA, or paced by the clock), and the web server + protocol pump in the main thread.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform_host.h"
#include "sloop.h"
#include "sloop_platform.h"
#include "sloop_proto.h"
#include "ws_server.h"

#ifdef SLOOPY_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static struct {
    int port;
    const char *bind, *web, *store, *audio, *device, *wav;
    double seconds;
} opt = {8080, "127.0.0.1", NULL, "build-host/sloop-store.bin", "alsa", "default", NULL, 0};

/* ------------------------------------------------------------------ UI --- */
static volatile int g_booted;

static void *ui_thread(void *arg)
{
    (void)arg;
    sloop_boot();
    g_booted = 1;
    while (!g_stop) {
        uint32_t ms = sloop_ui_step();
        sloop_plat_sleep_us((ms ? ms : 1) * 1000u);
    }
    return NULL;
}

/* --------------------------------------------------------------- audio --- */
static FILE *g_wav;
static uint32_t g_wav_frames;
static uint64_t g_frames, g_late;

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t v;
    uint16_t s;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36u + frames * 4u; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    v = SLOOP_FS; fwrite(&v, 4, 1, f); v = SLOOP_FS * 4u; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4u; fwrite(&v, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

static void to_s16(const int32_t *in, int16_t *out, size_t frames)
{
    size_t i;
    for (i = 0; i < 2u * frames; i++) {
        int32_t v = in[i] >> 8;                      /* 24 -> 16 bit */
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static void *audio_thread(void *arg)
{
    int16_t s16[2u * SLOOP_HALF_FRAMES];
    const uint64_t half_us = (uint64_t)SLOOP_HALF_FRAMES * 1000000u / SLOOP_FS;
#ifdef SLOOPY_HAVE_ALSA
    snd_pcm_t *pcm = NULL;
#endif
    uint64_t t0;
    (void)arg;
    while (!g_booted && !g_stop)
        usleep(1000);
#ifdef SLOOPY_HAVE_ALSA
    if (!strcmp(opt.audio, "alsa")) {
        int err = snd_pcm_open(&pcm, opt.device, SND_PCM_STREAM_PLAYBACK, 0);
        if (!err)
            err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, SLOOP_FS, 1, 30000);
        if (err) {
            fprintf(stderr, "audio: ALSA %s: %s; playing silently (null output)\n", opt.device, snd_strerror(err));
            if (pcm)
                snd_pcm_close(pcm);
            pcm = NULL;
        } else {
            fprintf(stderr, "audio: ALSA %s, 44.1 kHz stereo, ~30 ms latency\n", opt.device);
        }
    }
#else
    if (!strcmp(opt.audio, "alsa"))
        fprintf(stderr, "audio: built without ALSA (install alsa-lib-devel); null output\n");
#endif
    t0 = sloop_plat_time_us();
    while (!g_stop) {
        const int32_t *half = sloop_audio_render();
        to_s16(half, s16, SLOOP_HALF_FRAMES);
        if (g_wav) {
            fwrite(s16, sizeof s16, 1, g_wav);
            g_wav_frames += SLOOP_HALF_FRAMES;
        }
        g_frames += SLOOP_HALF_FRAMES;
#ifdef SLOOPY_HAVE_ALSA
        if (pcm) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, s16, SLOOP_HALF_FRAMES);
            if (w < 0) {
                g_late++;
                snd_pcm_recover(pcm, (int)w, 1);
            }
            continue;
        }
#endif
        {   /* paced by the clock, like a DMA ring two halves deep */
            uint64_t due = t0 + (g_frames / SLOOP_HALF_FRAMES) * half_us, now = sloop_plat_time_us();
            if (now + 2u * half_us < due)
                usleep((useconds_t)(due - 2u * half_us - now));
            else if (now > due + half_us)
                g_late++;
        }
    }
#ifdef SLOOPY_HAVE_ALSA
    if (pcm) {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
    }
#endif
    return NULL;
}

/* ----------------------------------------------------------------- web --- */
static sloop_proto_hub_t g_hub;
static uint8_t g_frame[SLOOP_PROTO_FRAME_BYTES];

static int t_text(void *ctx, const char *s, size_t n) { return ws_send_text(ctx, s, n); }
static int t_bin(void *ctx, const uint8_t *p, size_t n) { return ws_send_bin(ctx, p, n); }
static void w_open(void *u, ws_conn_t *c)
{
    (void)u;
    if (sloop_proto_client_add(&g_hub, c) < 0)
        ws_send_text(c, "{\"v\":1,\"t\":\"err\",\"msg\":\"too many clients\"}", 44);
    else
        fprintf(stderr, "web: panel connected (%d)\n", sloop_proto_clients(&g_hub));
}
static void w_text(void *u, ws_conn_t *c, const char *s, size_t n)
{
    (void)u;
    sloop_proto_on_text(&g_hub, c, s, n);
}
static void w_close(void *u, ws_conn_t *c)
{
    (void)u;
    sloop_proto_client_remove(&g_hub, c);
    fprintf(stderr, "web: panel disconnected (%d)\n", sloop_proto_clients(&g_hub));
}

static size_t api(const char *path, char *out, size_t cap)   /* the host's /api/status (as the firmware's) */
{
    sloop_status_t st;
    uint32_t r, e, w;
    int n;
    if (strcmp(path, "/api/status"))
        return 0;
    sloop_status_get(&st);
    host_store_stats(&r, &e, &w);
    n = snprintf(out, cap,
                 "{\"sloop\":\"%s\",\"upstream\":\"%s\",\"boot\":1,\"uptime_ms\":%lu,\"playing\":%u,\"bpm\":%d,"
                 "\"app\":{\"version\":\"host\",\"built\":\"%s %s\",\"partition\":\"host\",\"ota_state\":\"none\",\"ota\":0},"
                 "\"audio\":{\"output\":\"%s\",\"frames\":%llu,\"underruns\":%llu,\"late\":%llu,\"render_max_us\":0,"
                 "\"cpu_pct\":%u,\"peak\":0},\"heap\":{\"internal_free\":0,\"internal_min\":0,\"psram_free\":0},"
                 "\"stack_free\":{\"audio\":0,\"ui\":0},\"store\":{\"reads\":%lu,\"erases\":%lu,\"writes\":%lu,\"errors\":0,\"backup\":0},"
                 "\"web\":{\"panels\":%d,\"http\":0,\"ws_messages\":0,\"rects\":%lu},"
                 "\"usb_midi\":{\"enabled\":0,\"linked\":0,\"links\":0,\"rx\":0,\"rx_sysex\":0,\"rx_dropped\":0,"
                 "\"tx\":0,\"tx_dropped\":0}}",
                 sloop_version(), sloop_upstream_commit(), (unsigned long)st.uptime_ms, st.playing, st.bpm, __DATE__,
                 __TIME__, opt.audio,
                 (unsigned long long)g_frames, (unsigned long long)g_late, (unsigned long long)g_late,
                 (unsigned)(st.cpu_q8 * 100u / 256u), (unsigned long)r, (unsigned long)e, (unsigned long)w,
                 sloop_proto_clients(&g_hub), (unsigned long)g_hub.rects);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static const char *default_web(const char *argv0)
{
    static char path[1024];
    const char *cands[] = {"web", "../web", NULL};
    int i;
    for (i = 0; cands[i]; i++)
        if (!access(cands[i], R_OK)) {
            snprintf(path, sizeof path, "%s", cands[i]);
            return path;
        }
    snprintf(path, sizeof path, "%s/../web", argv0);
    return path;
}

static void usage(void)
{
    fprintf(stderr,
            "sloop_host: SLOOP on Linux with the browser FM-1 panel\n"
            "  --port N         web panel port (default 8080)\n"
            "  --bind ADDR      listen address (default 127.0.0.1; 0.0.0.0 for the LAN)\n"
            "  --web DIR        the panel's files (default ./web)\n"
            "  --store FILE     SLOOP's flash (settings, projects); default build-host/sloop-store.bin\n"
            "  --audio alsa|null  output (default alsa, falls back to null)\n"
            "  --device DEV     ALSA device (default \"default\")\n"
            "  --wav FILE       also record the output\n"
            "  --seconds S      stop after S seconds (tests)\n");
}

int main(int argc, char **argv)
{
    pthread_t ui, au;
    ws_server_t *srv;
    ws_handlers_t h = {w_open, w_text, w_close};
    sloop_proto_ops_t ops = {t_text, t_bin};
    uint64_t t_start, t_pump = 0;
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--port") && v) opt.port = atoi(v), i++;
        else if (!strcmp(a, "--bind") && v) opt.bind = v, i++;
        else if (!strcmp(a, "--web") && v) opt.web = v, i++;
        else if (!strcmp(a, "--store") && v) opt.store = v, i++;
        else if (!strcmp(a, "--audio") && v) opt.audio = v, i++;
        else if (!strcmp(a, "--device") && v) opt.device = v, i++;
        else if (!strcmp(a, "--wav") && v) opt.wav = v, i++;
        else if (!strcmp(a, "--seconds") && v) opt.seconds = atof(v), i++;
        else { usage(); return a[2] == 'h' ? 0 : 2; }
    }
    if (!opt.web)
        opt.web = default_web(argv[0]);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (host_store_open(!strcmp(opt.store, "none") ? NULL : opt.store)) {
        fprintf(stderr, "store: cannot open %s\n", opt.store);
        return 1;
    }
    if (opt.wav && (g_wav = fopen(opt.wav, "wb")))
        wav_header(g_wav, 0);
    sloop_proto_hub_init(&g_hub, &ops, "host", g_frame);
    srv = ws_server_start(opt.bind, opt.port, opt.web, &h, NULL);
    if (!srv)
        return 1;
#ifdef SLOOPY_WEB_GEN
    ws_server_add_root(srv, SLOOPY_WEB_GEN);          /* SLOOP's web editor (/editor.html) */
#endif
    ws_server_set_api(srv, api);
    fprintf(stderr, "sloop_host: %s (upstream %s), store %s\n", sloop_version(), sloop_upstream_commit(), opt.store);
    fprintf(stderr, "sloop_host: panel at http://%s:%d/  (web root %s)\n",
            strcmp(opt.bind, "0.0.0.0") ? opt.bind : "<this machine>", ws_server_port(srv), opt.web);
    pthread_create(&ui, NULL, ui_thread, NULL);
    pthread_create(&au, NULL, audio_thread, NULL);
    t_start = sloop_plat_time_us();
    while (!g_stop) {
        uint64_t now;
        ws_server_poll(srv, 5);
        now = sloop_plat_time_us();
        if (now - t_pump >= 33000u) {                 /* ~30 screen updates / s */
            t_pump = now;
            sloop_proto_pump(&g_hub);
        }
        if (opt.seconds > 0 && (double)(now - t_start) / 1e6 >= opt.seconds)
            g_stop = 1;
    }
    pthread_join(au, NULL);
    pthread_join(ui, NULL);
    ws_server_stop(srv);
    if (g_wav) {
        wav_header(g_wav, g_wav_frames);
        fclose(g_wav);
    }
    host_store_close();
    fprintf(stderr, "sloop_host: stopped (%llu frames, %llu late)\n", (unsigned long long)g_frames,
            (unsigned long long)g_late);
    return 0;
}
