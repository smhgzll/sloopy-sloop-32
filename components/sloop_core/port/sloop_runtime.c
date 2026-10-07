/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: stands in for upstream main.c (the FM-1's boot, interrupt wiring and main
 * loop) and implements the sloop.h API. Included last by sloop_unity.c.
 *
 * What upstream main.c does and where it went:
 *   fm1_cstart: clocks, watchdog, boot-loop guard, fatal vectors, RAM init, guards
 *        -> the platform (ESP-IDF startup / the host OS)
 *   fm1_main before the loop: persist_boot .. audio_init, the 900 ms logo
 *        -> sloop_boot() + the splash phase of sloop_ui_step()
 *   fm1_main loop body: MASTER pot, editor service, ui_input / ui_leds / ui_draw, autosave,
 *        sections flush, ~15 ms frames with ui_input in between -> sloop_ui_step()
 *   TIMER5 ISR: input scan, USB poll, fm1_ms -> port_clock() / port_apply_events() (UI context)
 *   ALNK0 ISR (audio.c, unchanged) -> run by sloop_audio_render() through the virtual ALNK
 * Not ported (FM-1 hardware only): battery ADC, OCT- + OCT+ "UPDATE MODE" (JieLi UBOOT), USB
 * UBOOT / M-UPGRADE requests, CDC console, boot-loop guard, crash screen (the platform reports).
 */
#ifndef SLOOP_UPSTREAM_COMMIT
#define SLOOP_UPSTREAM_COMMIT "unknown"
#endif
#define PORT_FRAME_MS 15u          /* main loop frame: ~60 UI frames / s at most (main.c) */
#define PORT_SPLASH_MS 930u        /* the logo stays a moment (main.c: 30 + 900 ms) */
#define PORT_MIN_HOLD_MS 20u       /* a key / button is down at least this long (see port_apply_events) */

/* power-on: three parts with their default sounds (TRK_DEF), the drum track, empty patterns
 * (upstream main.c felucca_init, unchanged) */
static void felucca_init(void)
{
    uint32_t i;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        track_defaults(t);
        if (i < NPART) {
            set_engine_of(t, TRK_DEF[i][0]);
            apply_preset_to(t, TRK_DEF[i][1]);   /* with its sends */
            t->engine = t->eng_req;
        }
        track_defaults_steps(t);              /* the sequencers start empty */
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;        /* the 808 kit */
    song.sel = 0;
    song.master_q12 = 2048;
    autosave_resume();                        /* the project as it was left (project.c) */
    layers_init();                            /* the panel's layer buttons for the keys (ui_layers.c) */
    go_home();
    ui.force = 1;
}

/* ------------------------------------------------------------------ names --- */
static const char *const PORT_BTN_NAME[SLOOP_BTN_COUNT] = {
    "FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"};
static const char *const PORT_ENC_NAME[SLOOP_ENC_COUNT] = {
    "SELECT", "ALGORITHM", "PRESETS", "K1", "K2", "K3", "K4"};
_Static_assert((int)SLOOP_BTN_COUNT == (int)NB && (int)SLOOP_ENC_COUNT == (int)NE, "sloop.h controls = panel.c labels");
_Static_assert(SLOOP_HALF_FRAMES == HALF_FRAMES && SLOOP_FS == FS, "sloop.h audio format = SLOOP's");

const char *sloop_btn_name(unsigned b) { return b < SLOOP_BTN_COUNT ? PORT_BTN_NAME[b] : 0; }
const char *sloop_enc_name(unsigned e) { return e < SLOOP_ENC_COUNT ? PORT_ENC_NAME[e] : 0; }
static int port_name_eq(const char *name, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!name[i] || name[i] != s[i])
            return 0;
    return name[n] == 0;
}
int sloop_btn_from_name(const char *s, size_t n)
{
    unsigned b;
    for (b = 0; b < SLOOP_BTN_COUNT; b++)
        if (port_name_eq(PORT_BTN_NAME[b], s, n))
            return (int)b;
    return -1;
}
int sloop_enc_from_name(const char *s, size_t n)
{
    unsigned e;
    for (e = 0; e < SLOOP_ENC_COUNT; e++)
        if (port_name_eq(PORT_ENC_NAME[e], s, n))
            return (int)e;
    return -1;
}

/* ------------------------------------------------------------------ clock --- */
static void port_clock(void)       /* fm1_ms (the TIMER5 ISR's job); UI context only: one writer */
{
    fm1_ms = (uint32_t)(sloop_plat_time_us() / 1000u);
}

/* --------------------------------------------------------- input mailbox --- */
/* Events posted by any task; applied in order by the UI context, which is where the FM-1's
 * input ISR would have changed the matrix. The controls are wired as on the real panel
 * (PANEL_DEFAULT), so HARDWARE CALIBRATION and the learned table behave as on the device.
 * SLOOP sees key levels in the audio ISR (seq.c) and button levels at UI frames, as the
 * matrix debounce guarantees on the FM-1: a press is held at least PORT_MIN_HOLD_MS, and a
 * control changes at most once per UI pass, however close together the events arrived. */
enum { EV_MATRIX, EV_ENC, EV_POT, EV_MIDI };
typedef struct {
    uint8_t type, id;
    int16_t val;
    uint32_t data;
} port_ev_t;
#define PORT_EVQ 256u
static port_ev_t port_evq[PORT_EVQ];
static uint32_t port_evq_w, port_evq_r;          /* (io lock) */
static uint32_t port_down_ms[FM1_NKEY];
static uint32_t port_ev_dropped;
/* Encoder detents reach SLOOP one per UI pass, as from a hand on a physical encoder: SLOOP reads
 * some encoders a step per read (ALGORITHM: one track per read) and accelerates turns whose reads
 * come < 60 ms apart (ui_input.c accel). A message of several detents is a fast turn. */
static int32_t port_enc_pending[FM1_NENC];

static int port_post(uint32_t type, uint32_t id, int32_t val, uint32_t data)
{
    int rc = -1;
    sloop_plat_io_lock();
    if (port_evq_w - port_evq_r < PORT_EVQ) {
        port_ev_t *e = &port_evq[port_evq_w % PORT_EVQ];
        e->type = (uint8_t)type;
        e->id = (uint8_t)id;
        e->val = (int16_t)val;
        e->data = data;
        port_evq_w++;
        rc = 0;
    } else {
        port_ev_dropped++;
    }
    sloop_plat_io_unlock();
    return rc;
}

int sloop_post_button(unsigned btn, int down)
{
    return btn < SLOOP_BTN_COUNT ? port_post(EV_MATRIX, PANEL_DEFAULT.btn[btn], down != 0, 0) : -1;
}
int sloop_post_key(unsigned key, int down)
{
    return key < SLOOP_NKEYS ? port_post(EV_MATRIX, 14u + key, down != 0, 0) : -1;
}
int sloop_post_encoder(unsigned enc, int steps)
{
    if (enc >= SLOOP_ENC_COUNT || !steps)
        return enc < SLOOP_ENC_COUNT ? 0 : -1;
    steps = steps > 64 ? 64 : steps < -64 ? -64 : steps;
    return port_post(EV_ENC, PANEL_DEFAULT.enc[enc], steps * PANEL_DEFAULT.dir[enc], 0);
}
int sloop_post_pot(unsigned pot, unsigned value)
{
    if (pot != SLOOP_POT_MASTER)
        return -1;
    return port_post(EV_POT, FM1_ADC_MASTER, (int32_t)(value > 1023u ? 1023u : value), 0);
}
int sloop_post_midi(uint32_t pkt) { return port_post(EV_MIDI, 0, 0, pkt); }

static void port_apply_events(void)
{
    uint64_t touched = 0;
    for (;;) {
        port_ev_t e;
        sloop_plat_io_lock();
        if (port_evq_r == port_evq_w) {
            sloop_plat_io_unlock();
            return;
        }
        e = port_evq[port_evq_r % PORT_EVQ];
        if (e.type == EV_MATRIX && e.id < FM1_NKEY) {
            uint32_t id = e.id, is_down = id >= 14u ? (fm1_in.notes >> (id - 14u)) & 1u : (fm1_in.buttons >> id) & 1u;
            if (((touched >> id) & 1u) ||
                (!e.val && is_down && (uint32_t)(fm1_ms - port_down_ms[id]) < PORT_MIN_HOLD_MS)) {
                sloop_plat_io_unlock();             /* this one waits for a later pass, and so does the rest */
                return;
            }
        }
        if (e.type == EV_MIDI && mi_w - mi_r >= MQ) {
            sloop_plat_io_unlock();                 /* the audio ISR drains the ring: later */
            return;
        }
        port_evq_r++;
        sloop_plat_io_unlock();
        switch (e.type) {
        case EV_MATRIX:
            if (e.id < FM1_NKEY) {
                touched |= (uint64_t)1 << e.id;
                if (e.val)
                    port_down_ms[e.id] = fm1_ms;
                fm1_virt_key(e.id, e.val);
                ui_input_ms = fm1_ms;
            }
            break;
        case EV_ENC:
            if (e.id < FM1_NENC)
                port_enc_pending[e.id] += e.val;
            break;
        case EV_POT:
            if (e.id < FM1_ADC_NCH)
                fm1_adc_virt[e.id] = e.val;
            break;
        case EV_MIDI:                               /* single producer (here), the audio ISR consumes */
            midi_in_q[mi_w % MQ] = e.data;
            RING_PUBLISH();
            mi_w++;
            break;
        }
    }
}

static void port_feed_encoders(void)
{
    uint32_t e;
    for (e = 0; e < FM1_NENC; e++)
        if (port_enc_pending[e]) {
            int32_t step = port_enc_pending[e] > 0 ? 1 : -1;
            port_enc_pending[e] -= step;
            fm1_virt_enc(e, step);
        }
}

/* ------------------------------------------------------ SysEx and MIDI out --- */
/* SysEx requests (the web editor) wait in a small inbox until editor.c's frame buffer (usb.c's
 * sx_frame, one frame at a time) is free; the UI step moves them in. Replies leave through usb.c's
 * SysEx packet ring (ota_wire_send), notes through midi_out_q; every attached transport reads both
 * (its own task, its own position: sloop_usb_shim.c). Requests from several transports share the
 * inbox, and every attached transport gets every reply and push, as every editor on the hub does. */
#define PORT_SXIN 4u
static uint8_t port_sxin[PORT_SXIN][SLOOP_SYSEX_MAX];
static uint16_t port_sxin_len[PORT_SXIN];
static uint32_t port_sxin_w, port_sxin_r;           /* (io lock) */
static uint8_t port_sx_msg[SLOOP_MIDI_PORTS][SLOOP_SYSEX_MAX + 2u]    /* a reply being reassembled, per transport */
    __attribute__((section(".pool")));
static size_t port_sx_n[SLOOP_MIDI_PORTS];

void sloop_midi_attach(unsigned port, int attached)
{
    uint32_t bit = 1u << port;
    if (port >= SLOOP_MIDI_PORTS)
        return;
    sloop_plat_io_lock();
    if (attached && !(port_midi_on & bit)) {
        port_mo_r[port] = mo_w;                         /* from now on: nothing stale */
        port_so_r[port] = so_w;
        port_sx_n[port] = 0;
        RING_PUBLISH();
        port_midi_on |= bit;
    } else if (!attached) {
        port_midi_on &= ~bit;
    }
    usb.config = port_midi_on ? 1u : 0u;
    sloop_plat_io_unlock();
}

int sloop_post_sysex(const uint8_t *msg, size_t n)
{
    int rc = -1;
    size_t i;
    if (n < 2u || msg[0] != 0xF0u || msg[n - 1u] != 0xF7u || n - 2u > SLOOP_SYSEX_MAX)
        return -1;
    for (i = 1; i + 1u < n; i++)
        if (msg[i] & 0x80u)
            return -1;                                  /* (7-bit data only between F0 and F7) */
    sloop_plat_io_lock();
    if (port_sxin_w - port_sxin_r < PORT_SXIN) {
        uint32_t k = port_sxin_w % PORT_SXIN;
        memcpy(port_sxin[k], msg + 1, n - 2u);
        port_sxin_len[k] = (uint16_t)(n - 2u);
        port_sxin_w++;
        rc = 0;
    }
    sloop_plat_io_unlock();
    return rc;
}

static void port_feed_sysex(void)                       /* UI context: the next request to editor.c */
{
    if (sx_ready)
        return;
    sloop_plat_io_lock();
    if (port_sxin_r != port_sxin_w) {
        uint32_t k = port_sxin_r % PORT_SXIN;
        memcpy(sx_frame, port_sxin[k], port_sxin_len[k]);
        sx_frame_len = port_sxin_len[k];
        port_sxin_r++;
        RING_PUBLISH();
        sx_ready = 1;                                   /* (as usb.c on the frame's F7) */
    }
    sloop_plat_io_unlock();
}

size_t sloop_take_sysex(unsigned port, uint8_t *buf, size_t cap)
{
    uint8_t *msg = port_sx_msg[port < SLOOP_MIDI_PORTS ? port : 0];
    size_t n;
    if (port >= SLOOP_MIDI_PORTS || !((port_midi_on >> port) & 1u))
        return 0;
    n = port_sx_n[port];
    while (port_so_r[port] != so_w) {
        uint32_t pkt = sx_out_q[port_so_r[port] % SXQ], cin = pkt & 0x0Fu, k, i;
        RING_PUBLISH();
        port_so_r[port]++;
        k = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : cin == 5u ? 1u : 0u;
        for (i = 0; i < k; i++) {
            uint8_t b = (uint8_t)(pkt >> (8u + 8u * i));
            if (b == 0xF0u)
                n = 0;
            if (n < SLOOP_SYSEX_MAX + 2u)
                msg[n++] = b;
        }
        if (cin >= 5u && cin <= 7u) {                   /* the end of a message */
            size_t m = n;
            n = 0;
            if (m >= 2u && msg[0] == 0xF0u && msg[m - 1u] == 0xF7u && m <= cap) {
                port_sx_n[port] = 0;
                memcpy(buf, msg, m);
                return m;
            }
        }
    }
    port_sx_n[port] = n;
    return 0;
}

int sloop_take_midi(unsigned port, uint32_t *pkt)
{
    if (port >= SLOOP_MIDI_PORTS || !((port_midi_on >> port) & 1u) || port_mo_r[port] == mo_w)
        return 0;
    *pkt = midi_out_q[port_mo_r[port] % MQ];
    RING_PUBLISH();
    port_mo_r[port]++;
    return 1;
}

/* ---------------------------------------------------------- store images --- */
/* an object of storage.c, copy A or B, in an image of the store window: its header and payload
 * checked as st_head / st_body check them on flash */
static int port_img_obj_ok(const uint8_t *img, uint32_t obj, uint32_t copy)
{
    st_hdr_t h;
    uint32_t off = st_sector(obj, copy) - SLOOP_STORE_LO;
    memcpy(&h, img + off, sizeof h);
    return h.magic == ST_MAGIC && h.type == obj && h.len <= ST_PAYLOAD_MAX &&
           h.hcrc == st_crc32(&h, sizeof h - 4u) && st_crc32(img + off + ST_PAYLOAD_OFF, h.len) == h.crc;
}

int sloop_store_check(const uint8_t *img, size_t n, sloop_store_info_t *info)
{
    sloop_store_info_t z = {0};
    uint32_t obj, k, i;
    if (info)
        *info = z;
    if (!img || n != SLOOP_STORE_BYTES)
        return 0;
    for (obj = 0; obj < OBJ_COUNT; obj++) {
        if (!port_img_obj_ok(img, obj, 0) && !port_img_obj_ok(img, obj, 1))
            continue;
        if (obj == OBJ_SETTINGS)
            z.settings = 1;
        else if (obj == OBJ_AUTOSAVE)
            z.autosave = 1;
        else if (obj >= OBJ_UPRESET0)
            z.preset_banks++;
        else
            z.projects++;
    }
    for (k = 0; k < SMP_USER_SLOTS; k++) {        /* the header checks of eng_sample.c's smp_user_scan */
        smp_user_hdr_t h;
        int ok;
        memcpy(&h, img + SMP_USER_BASE + k * SMP_USER_SIZE - SLOOP_STORE_LO, sizeof h);
        ok = h.magic == SMP_USER_MAGIC && h.version == 1 && h.nz && h.nz <= 16u &&
             h.data_len <= SMP_USER_SIZE - SMP_USER_DATA;
        for (i = 0; ok && i < h.nz; i++)
            ok = smp_zone_ok(&h.zone[i], h.data_len);
        z.samples += ok ? 1u : 0u;
    }
    if (info)
        *info = z;
    return z.settings || z.projects || z.autosave || z.preset_banks || z.samples;
}

/* ------------------------------------------------------- display mailbox --- */
/* The virtual ST7789 draws into fm1_vlcd_fb (UI context). Whatever changed is copied to the
 * published screen under the io lock, so transports never see half a frame. */
static uint8_t port_pub_fb[SLOOP_LCD_BYTES] __attribute__((section(".pool"), aligned(4)));
static sloop_rect_t port_pub_dirty;
static uint8_t port_pub_has;
static uint32_t port_pub_frames;

static void port_publish_display(void)
{
    uint32_t x0 = fm1_vlcd.dx0, y0 = fm1_vlcd.dy0, x1 = fm1_vlcd.dx1, y1 = fm1_vlcd.dy1, y;
    if (x0 > x1)
        return;
    fm1_vlcd.dx0 = fm1_vlcd.dy0 = 0xFFFF;
    fm1_vlcd.dx1 = fm1_vlcd.dy1 = 0;
    sloop_plat_io_lock();
    for (y = y0; y <= y1; y++)
        memcpy(&port_pub_fb[(y * SLOOP_LCD_W + x0) * 2u], &fm1_vlcd_fb[(y * SLOOP_LCD_W + x0) * 2u], (x1 - x0 + 1u) * 2u);
    if (!port_pub_has) {
        port_pub_dirty.x = (uint16_t)x0;
        port_pub_dirty.y = (uint16_t)y0;
        port_pub_dirty.w = (uint16_t)(x1 - x0 + 1u);
        port_pub_dirty.h = (uint16_t)(y1 - y0 + 1u);
        port_pub_has = 1;
    } else {
        uint32_t ox1 = port_pub_dirty.x + port_pub_dirty.w - 1u, oy1 = port_pub_dirty.y + port_pub_dirty.h - 1u;
        uint32_t nx0 = x0 < port_pub_dirty.x ? x0 : port_pub_dirty.x, ny0 = y0 < port_pub_dirty.y ? y0 : port_pub_dirty.y;
        uint32_t nx1 = x1 > ox1 ? x1 : ox1, ny1 = y1 > oy1 ? y1 : oy1;
        port_pub_dirty.x = (uint16_t)nx0;
        port_pub_dirty.y = (uint16_t)ny0;
        port_pub_dirty.w = (uint16_t)(nx1 - nx0 + 1u);
        port_pub_dirty.h = (uint16_t)(ny1 - ny0 + 1u);
    }
    port_pub_frames++;
    sloop_plat_io_unlock();
}

int sloop_display_take(sloop_rect_t *r, uint8_t *dst, size_t cap)
{
    uint32_t y;
    sloop_plat_io_lock();
    if (!port_pub_has) {
        sloop_plat_io_unlock();
        return 0;
    }
    *r = port_pub_dirty;
    if (!dst || cap < (size_t)r->w * r->h * 2u) {
        sloop_plat_io_unlock();
        return -1;
    }
    for (y = 0; y < r->h; y++)
        memcpy(dst + (size_t)y * r->w * 2u, &port_pub_fb[((r->y + y) * SLOOP_LCD_W + r->x) * 2u], (size_t)r->w * 2u);
    port_pub_has = 0;
    sloop_plat_io_unlock();
    return 1;
}

void sloop_display_copy_full(uint8_t *dst)
{
    sloop_plat_io_lock();
    memcpy(dst, port_pub_fb, SLOOP_LCD_BYTES);
    sloop_plat_io_unlock();
}

uint32_t sloop_display_frames(void)
{
    uint32_t n;
    sloop_plat_io_lock();
    n = port_pub_frames;
    sloop_plat_io_unlock();
    return n;
}

/* ------------------------------------------------------- LEDs and status --- */
static uint8_t port_led[SLOOP_LED_COUNT];
static uint32_t port_led_ver;
static sloop_status_t port_status;
static uint32_t port_ui_frames;

static void port_publish_leds(void)
{
    uint8_t now[SLOOP_LED_COUNT];
    uint32_t i;
    for (i = 0; i < SLOOP_BTN_COUNT; i++)      /* the LED of the physical button with that label */
        now[i] = (uint8_t)fm1_virt_led(PANEL_DEFAULT.btn[i]);
    for (i = 0; i < SLOOP_NKEYS; i++)
        now[SLOOP_BTN_COUNT + i] = (uint8_t)fm1_virt_led(14u + i);
    sloop_plat_io_lock();
    if (memcmp(now, port_led, sizeof now)) {
        memcpy(port_led, now, sizeof now);
        port_led_ver++;
    }
    sloop_plat_io_unlock();
}

uint32_t sloop_leds_get(uint8_t out[SLOOP_LED_COUNT])
{
    uint32_t v;
    sloop_plat_io_lock();
    memcpy(out, port_led, SLOOP_LED_COUNT);
    v = port_led_ver;
    sloop_plat_io_unlock();
    return v;
}

static void port_publish_status(void)
{
    sloop_status_t s = port_status;
    s.playing = song.playing != 0;
    s.recording = song.rec != 0;
    s.sel_track = song.sel;
    s.page = (uint8_t)(ui.home ? 0xFFu : ui.page);
    s.menu = ui.menu != 0;
    s.bpm = song.g[G_BPM];
    s.cpu_q8 = (uint16_t)(song.cpu_q8 > 0xFFFFu ? 0xFFFFu : song.cpu_q8);
    s.audio_halves = audio_halves;
    s.ui_frames = port_ui_frames;
    s.uptime_ms = fm1_ms;
    sloop_plat_io_lock();
    if (s.playing != port_status.playing || s.recording != port_status.recording ||
        s.sel_track != port_status.sel_track || s.page != port_status.page || s.bpm != port_status.bpm ||
        s.menu != port_status.menu)
        s.version = port_status.version + 1u;
    port_status = s;
    sloop_plat_io_unlock();
}

void sloop_status_get(sloop_status_t *st)
{
    sloop_plat_io_lock();
    *st = port_status;
    sloop_plat_io_unlock();
}

int sloop_param_get(unsigned track, const char *label, int *value)
{
    uint32_t i;
    if (track == SLOOP_GLOBAL) {
        for (i = 0; i < G_COUNT; i++)
            if (GP[i].label && str_eq(GP[i].label, label)) {
                *value = song.g[i];
                return 1;
            }
        return 0;
    }
    if (track >= NTRK)
        return 0;
    for (i = 0; i < P_E0; i++)                          /* (P_E0..: engine-specific, labels vary) */
        if (TP[i].label && str_eq(TP[i].label, label)) {
            *value = trk[track].p[i];
            return 1;
        }
    return 0;
}

const char *sloop_version(void) { return FELUCCA_VERSION; }
const char *sloop_upstream_commit(void) { return SLOOP_UPSTREAM_COMMIT; }

/* ------------------------------------------------------------- lifecycle --- */
static uint8_t port_booted, port_splash;
static uint32_t port_boot_ms, port_frame_ms;
static int32_t port_knob = 512 * 16;

void sloop_boot(void)
{
    port_clock();
    persist_boot();
    settings_init();
    lcd_init();
    sloop_splash();                                     /* the SLOOP logo (splash.c) */
    if (felucca_dbg.magic != DBG_MAGIC) {
        memset(&felucca_dbg, 0, sizeof felucca_dbg);
        felucca_dbg.magic = DBG_MAGIC;
    }
    felucca_dbg.boots++;
    felucca_dbg.max_us = 0;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    felucca_init();
    audio_init();
    usb_start();
    port_boot_ms = fm1_ms;
    port_splash = 1;
    port_booted = 1;
    port_publish_display();
    port_publish_leds();
    port_publish_status();
}

static void port_frame(void)                            /* main.c: the loop body */
{
    {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            uint32_t k10;
            port_knob += (a * 16 - port_knob) / 8;
            k10 = (uint32_t)(port_knob / 16);
            song.master_q12 = (k10 * k10) >> 8;            /* 0 .. ~4096 */
        }
    }
    port_feed_sysex();
    ed_service();                                       /* web editor SysEx */
    felucca_dbg.ui_frames++;
    felucca_dbg.page = ui.page;
    felucca_dbg.home = ui.home;
    felucca_dbg.stage = 1;
    ui_input();
    felucca_dbg.stage = 2;
    ui_leds();
    ui_draw();
    felucca_dbg.stage = 8;
    autosave_tick();                                    /* the working project into flash, when quiet */
    sections_flush();                                   /* live sections / the recorded song, when quiet */
    felucca_dbg.stage = 9;
    port_ui_frames++;
    port_publish_leds();
    port_publish_status();
}

uint32_t sloop_ui_step(void)
{
    if (!port_booted)
        return 10;
    port_clock();
    port_apply_events();
    port_feed_encoders();
    if (port_splash) {
        if ((uint32_t)(fm1_ms - port_boot_ms) < PORT_SPLASH_MS)
            return 5;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        port_splash = 0;
        port_frame_ms = fm1_ms - PORT_FRAME_MS;
    }
    if ((uint32_t)(fm1_ms - port_frame_ms) >= PORT_FRAME_MS) {
        port_frame_ms = fm1_ms;
        port_frame();
    } else {
        port_feed_sysex();
        ui_input();                                     /* main.c: between frames */
        ed_service();
    }
    port_publish_display();
    return 1;
}

/* upstream loops that wait for the input ISR feed the watchdog (fm1_sys.h) */
static void sloop_port_idle(void)
{
    sloop_plat_sleep_us(1000);
    port_clock();
    port_apply_events();
    port_feed_encoders();
    port_publish_display();
}

/* the ISR entry of audio.c's fm1_alnk0_irq (hal/fm1_isr.S on the FM-1) */
void isr_alnk0(void) { fm1_alnk0_irq(); }

static const int32_t port_silence[2u * SLOOP_HALF_FRAMES];

const int32_t *sloop_audio_render(void)
{
    const int32_t *o;
    sloop_plat_render_begin();
    o = fm1_alnk_next();
    sloop_plat_render_end();
    return o ? o : port_silence;
}
