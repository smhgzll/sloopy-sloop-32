/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the web panel protocol (see sloop_proto.h, docs/PROTOCOL.md). */
#include "sloop_proto.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- the parser --- */
/* Client messages are flat JSON objects: string, integer, boolean and null values, and arrays of
 * integers. Anything else (nested objects, fractions, exponents) is rejected; unknown keys are
 * skipped. Strings keep \" \\ \/ escapes; \uXXXX is accepted for ASCII only. */
typedef struct {
    const char *p, *e;
} cur_t;

static void ws(cur_t *c)
{
    while (c->p < c->e && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static int lit(cur_t *c, char ch)
{
    ws(c);
    if (c->p < c->e && *c->p == ch) {
        c->p++;
        return 1;
    }
    return 0;
}

static int word(cur_t *c, const char *w)
{
    size_t n = strlen(w);
    ws(c);
    if ((size_t)(c->e - c->p) >= n && !memcmp(c->p, w, n)) {
        c->p += n;
        return 1;
    }
    return 0;
}

static int hexv(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

/* a string into dst (truncated to cap - 1); 0 ok */
static int pstr(cur_t *c, char *dst, size_t cap, size_t *len)
{
    size_t n = 0;
    if (!lit(c, '"'))
        return -1;
    while (c->p < c->e && *c->p != '"') {
        char ch = *c->p++;
        if ((unsigned char)ch < 0x20)
            return -1;
        if (ch == '\\') {
            if (c->p >= c->e)
                return -1;
            ch = *c->p++;
            if (ch == 'u') {
                int v = 0, i;
                for (i = 0; i < 4; i++) {
                    int hx = c->p < c->e ? hexv(*c->p++) : -1;
                    if (hx < 0)
                        return -1;
                    v = v << 4 | hx;
                }
                if (v > 0x7F)
                    return -1;
                ch = (char)v;
            } else if (ch == 'n') {
                ch = '\n';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch != '"' && ch != '\\' && ch != '/') {
                return -1;
            }
        }
        if (n + 1 < cap)
            dst[n] = ch;
        n++;
    }
    if (!lit(c, '"'))
        return -1;
    if (cap)
        dst[n < cap ? n : cap - 1] = 0;
    if (len)
        *len = n;
    return 0;
}

/* a hex string ("F07D..F7") into bytes */
static int phex(cur_t *c, uint8_t *dst, size_t cap, uint16_t *len)
{
    size_t n = 0;
    if (!lit(c, '"'))
        return -1;
    while (c->p < c->e && *c->p != '"') {
        int hi = hexv(*c->p++), lo = c->p < c->e ? hexv(*c->p++) : -1;
        if (hi < 0 || lo < 0 || n >= cap)
            return -1;
        dst[n++] = (uint8_t)(hi << 4 | lo);
    }
    if (!lit(c, '"'))
        return -1;
    *len = (uint16_t)n;
    return 0;
}

static int pint(cur_t *c, long *v)
{
    long x = 0;
    int neg = 0, digits = 0;
    ws(c);
    if (c->p < c->e && *c->p == '-') {
        neg = 1;
        c->p++;
    }
    while (c->p < c->e && *c->p >= '0' && *c->p <= '9') {
        if (x > 100000000L)
            return -1;                                   /* (no value of the protocol is that big) */
        x = x * 10 + (*c->p++ - '0');
        digits++;
    }
    if (!digits || (c->p < c->e && (*c->p == '.' || *c->p == 'e' || *c->p == 'E')))
        return -1;
    *v = neg ? -x : x;
    return 0;
}

enum { V_NONE, V_STR, V_INT, V_BOOL, V_NULL, V_ARR };
typedef struct {
    int type;
    char s[24];
    long i;
    long a[4];
    int an;
} val_t;

static int pval(cur_t *c, val_t *v)
{
    ws(c);
    memset(v, 0, sizeof *v);
    if (c->p >= c->e)
        return -1;
    if (*c->p == '"') {
        v->type = V_STR;
        return pstr(c, v->s, sizeof v->s, NULL);
    }
    if (word(c, "true")) {
        v->type = V_BOOL;
        v->i = 1;
        return 0;
    }
    if (word(c, "false")) {
        v->type = V_BOOL;
        return 0;
    }
    if (word(c, "null")) {
        v->type = V_NULL;
        return 0;
    }
    if (lit(c, '[')) {
        v->type = V_ARR;
        if (lit(c, ']'))
            return 0;
        do {
            long x;
            if (pint(c, &x))
                return -1;
            if (v->an < 4)
                v->a[v->an] = x;
            v->an++;
        } while (lit(c, ','));
        return lit(c, ']') ? 0 : -1;
    }
    v->type = V_INT;
    return pint(c, &v->i);
}

static int fail(sloop_proto_msg_t *m, const char *why)
{
    size_t n = strlen(why);
    if (n >= sizeof m->err)
        n = sizeof m->err - 1;
    memcpy(m->err, why, n);
    m->err[n] = 0;
    m->kind = SLOOP_PROTO_NONE;
    return -1;
}

int sloop_proto_parse(const char *json, size_t len, sloop_proto_msg_t *m)
{
    cur_t c = {json, json + len};
    char t[16] = {0}, id[24] = {0};
    long ver = -1, k = -1, d = 0, val = -1, n = 0;
    int down = -1, have_d = 0, have_val = 0, have_n = 0;
    long data[4];
    int datan = -1, have_sx = 0, want_midi = 0, screen = 1;
    memset(m, 0, sizeof *m);
    if (!lit(&c, '{'))
        return fail(m, "not a JSON object");
    if (!lit(&c, '}')) {
        do {
            char key[16];
            val_t v;
            if (pstr(&c, key, sizeof key, NULL) || !lit(&c, ':'))
                return fail(m, "malformed JSON");
            ws(&c);
            if (!strcmp(key, "data") && c.p < c.e && *c.p == '"') {   /* SysEx bytes in hex */
                if (phex(&c, m->sx, sizeof m->sx, &m->sx_len))
                    return fail(m, "data: bad hex or too long");
                have_sx = 1;
                continue;
            }
            if (pval(&c, &v))
                return fail(m, "malformed JSON");
            if (!strcmp(key, "v") && v.type == V_INT)
                ver = v.i;
            else if (!strcmp(key, "t") && v.type == V_STR)
                memcpy(t, v.s, sizeof t - 1);
            else if (!strcmp(key, "id") && v.type == V_STR)
                memcpy(id, v.s, sizeof id - 1);
            else if (!strcmp(key, "down") && v.type == V_BOOL)
                down = (int)v.i;
            else if (!strcmp(key, "k") && v.type == V_INT)
                k = v.i;
            else if (!strcmp(key, "d") && v.type == V_INT)
                d = v.i, have_d = 1;
            else if (!strcmp(key, "val") && v.type == V_INT)
                val = v.i, have_val = 1;
            else if (!strcmp(key, "n") && v.type == V_INT)
                n = v.i, have_n = 1;
            else if (!strcmp(key, "midi") && v.type == V_BOOL)
                want_midi = (int)v.i;
            else if (!strcmp(key, "screen") && v.type == V_BOOL)
                screen = (int)v.i;
            else if (!strcmp(key, "data") && v.type == V_ARR) {
                datan = v.an;
                memcpy(data, v.a, sizeof data);
            }
        } while (lit(&c, ','));
        if (!lit(&c, '}'))
            return fail(m, "malformed JSON");
    }
    ws(&c);
    if (c.p != c.e)
        return fail(m, "trailing bytes after the object");
    if (ver != SLOOP_PROTO_VERSION)
        return fail(m, "unsupported or missing protocol version v");
    if (!t[0])
        return fail(m, "missing message type t");

    if (!strcmp(t, "hello")) {
        m->kind = SLOOP_PROTO_HELLO;
        m->want_midi = (uint8_t)want_midi;
        m->no_screen = (uint8_t)!screen;
    } else if (!strcmp(t, "sysex")) {
        if (!have_sx || m->sx_len < 2u || m->sx[0] != 0xF0u || m->sx[m->sx_len - 1u] != 0xF7u)
            return fail(m, "sysex: data must be one F0..F7 message in hex");
        m->kind = SLOOP_PROTO_SYSEX;
    } else if (!strcmp(t, "full")) {
        m->kind = SLOOP_PROTO_FULL;
    } else if (!strcmp(t, "ping")) {
        if (!have_n || n < 0)
            return fail(m, "ping: n missing");
        m->kind = SLOOP_PROTO_PING;
        m->n = (uint32_t)n;
    } else if (!strcmp(t, "btn")) {
        int b = sloop_btn_from_name(id, strlen(id));
        if (b < 0)
            return fail(m, "btn: unknown id");
        if (down < 0)
            return fail(m, "btn: down missing");
        m->kind = SLOOP_PROTO_BTN;
        m->id = b;
        m->down = down;
    } else if (!strcmp(t, "key")) {
        if (k < 0 || k >= (long)SLOOP_NKEYS)
            return fail(m, "key: k out of range");
        if (down < 0)
            return fail(m, "key: down missing");
        m->kind = SLOOP_PROTO_KEY;
        m->id = (int)k;
        m->down = down;
    } else if (!strcmp(t, "enc")) {
        int e = sloop_enc_from_name(id, strlen(id));
        if (e < 0)
            return fail(m, "enc: unknown id");
        if (!have_d || d == 0 || d < -64 || d > 64)
            return fail(m, "enc: d must be -64..64, not 0");
        m->kind = SLOOP_PROTO_ENC;
        m->id = e;
        m->value = (int)d;
    } else if (!strcmp(t, "pot")) {
        if (strcmp(id, "MASTER"))
            return fail(m, "pot: unknown id");
        if (!have_val || val < 0 || val > 1023)
            return fail(m, "pot: val must be 0..1023");
        m->kind = SLOOP_PROTO_POT;
        m->id = SLOOP_POT_MASTER;
        m->value = (int)val;
    } else if (!strcmp(t, "midi")) {
        int i, want;
        if (datan < 1 || datan > 3)
            return fail(m, "midi: data must hold 1..3 bytes");
        for (i = 0; i < datan; i++)
            if (data[i] < 0 || data[i] > 255 || (i && data[i] > 127))
                return fail(m, "midi: bad byte");
        if (data[0] < 0x80 || data[0] >= 0xF0)
            return fail(m, "midi: only channel messages");
        want = (data[0] & 0xF0) == 0xC0 || (data[0] & 0xF0) == 0xD0 ? 2 : 3;
        if (datan != want)
            return fail(m, "midi: wrong length for this status");
        m->kind = SLOOP_PROTO_MIDI;
        m->midi_len = (uint8_t)datan;
        for (i = 0; i < datan; i++)
            m->midi[i] = (uint8_t)data[i];
    } else {
        return fail(m, "unknown message type");
    }
    return 0;
}

int sloop_proto_apply(const sloop_proto_msg_t *m)
{
    switch (m->kind) {
    case SLOOP_PROTO_BTN:
        return sloop_post_button((unsigned)m->id, m->down);
    case SLOOP_PROTO_KEY:
        return sloop_post_key((unsigned)m->id, m->down);
    case SLOOP_PROTO_ENC:
        return sloop_post_encoder((unsigned)m->id, m->value);
    case SLOOP_PROTO_POT:
        return sloop_post_pot((unsigned)m->id, (unsigned)m->value);
    case SLOOP_PROTO_SYSEX:
        return sloop_post_sysex(m->sx, m->sx_len);
    case SLOOP_PROTO_MIDI: {
        /* USB-MIDI event packet, cable 0: CIN = status >> 4 */
        uint32_t st = m->midi[0];
        uint32_t pkt = (st >> 4) | st << 8 | (uint32_t)m->midi[1] << 16 | (m->midi_len > 2 ? (uint32_t)m->midi[2] << 24 : 0u);
        return sloop_post_midi(pkt);
    }
    default:
        return -1;
    }
}

/* -------------------------------------------------------------- encoders --- */
static size_t done(char *buf, size_t cap, int n)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap)
            buf[0] = 0;
        return 0;
    }
    return (size_t)n;
}

/* a string for JSON: only printable ASCII, no quotes or backslashes (all strings here are ours) */
static const char *safe(const char *s, char *tmp, size_t cap)
{
    size_t i;
    for (i = 0; s && s[i] && i + 1 < cap; i++)
        tmp[i] = (s[i] >= 0x20 && s[i] < 0x7F && s[i] != '"' && s[i] != '\\') ? s[i] : '?';
    tmp[i] = 0;
    return tmp;
}

size_t sloop_proto_hello(char *buf, size_t cap, const char *target)
{
    char a[40], b[40], c[24];
    size_t n;
    unsigned i;
    int w = snprintf(buf, cap,
                     "{\"v\":%d,\"t\":\"hello\",\"proto\":%d,\"fw\":\"sloopy-sloop-32\",\"sloop\":\"%s\",\"upstream\":\"%s\","
                     "\"target\":\"%s\",\"lcd\":[%u,%u],\"keys\":%u,\"pots\":[\"MASTER\"],\"buttons\":[",
                     SLOOP_PROTO_VERSION, SLOOP_PROTO_VERSION, safe(sloop_version(), a, sizeof a),
                     safe(sloop_upstream_commit(), b, sizeof b), safe(target, c, sizeof c), SLOOP_LCD_W, SLOOP_LCD_H,
                     SLOOP_NKEYS);
    if (!(n = done(buf, cap, w)))
        return 0;
    for (i = 0; i < SLOOP_BTN_COUNT; i++) {
        if (!(w = (int)done(buf + n, cap - n, snprintf(buf + n, cap - n, "%s\"%s\"", i ? "," : "", sloop_btn_name(i)))))
            return done(buf, cap, -1);
        n += (size_t)w;
    }
    if (!(w = (int)done(buf + n, cap - n, snprintf(buf + n, cap - n, "],\"encoders\":["))))
        return done(buf, cap, -1);
    n += (size_t)w;
    for (i = 0; i < SLOOP_ENC_COUNT; i++) {
        if (!(w = (int)done(buf + n, cap - n, snprintf(buf + n, cap - n, "%s\"%s\"", i ? "," : "", sloop_enc_name(i)))))
            return done(buf, cap, -1);
        n += (size_t)w;
    }
    if (!(w = (int)done(buf + n, cap - n, snprintf(buf + n, cap - n, "]}"))))
        return done(buf, cap, -1);
    return n + (size_t)w;
}

size_t sloop_proto_leds(char *buf, size_t cap, const uint8_t led[SLOOP_LED_COUNT])
{
    char s[SLOOP_LED_COUNT + 1];
    unsigned i;
    for (i = 0; i < SLOOP_LED_COUNT; i++)
        s[i] = (char)('0' + (led[i] > 2 ? 2 : led[i]));
    s[SLOOP_LED_COUNT] = 0;
    return done(buf, cap, snprintf(buf, cap, "{\"v\":%d,\"t\":\"leds\",\"s\":\"%s\"}", SLOOP_PROTO_VERSION, s));
}

size_t sloop_proto_status(char *buf, size_t cap, const sloop_status_t *st)
{
    return done(buf, cap,
                snprintf(buf, cap,
                         "{\"v\":%d,\"t\":\"status\",\"playing\":%u,\"rec\":%u,\"track\":%u,\"page\":%d,\"menu\":%u,"
                         "\"bpm\":%d,\"cpu\":%u,\"uptime\":%lu}",
                         SLOOP_PROTO_VERSION, st->playing, st->recording, st->sel_track,
                         st->page == 0xFFu ? -1 : (int)st->page, st->menu, st->bpm, (unsigned)(st->cpu_q8 * 100u / 256u),
                         (unsigned long)st->uptime_ms));
}

size_t sloop_proto_pong(char *buf, size_t cap, uint32_t n)
{
    return done(buf, cap, snprintf(buf, cap, "{\"v\":%d,\"t\":\"pong\",\"n\":%lu}", SLOOP_PROTO_VERSION, (unsigned long)n));
}

size_t sloop_proto_error(char *buf, size_t cap, const char *msg)
{
    char m[64];
    return done(buf, cap, snprintf(buf, cap, "{\"v\":%d,\"t\":\"err\",\"msg\":\"%s\"}", SLOOP_PROTO_VERSION, safe(msg, m, sizeof m)));
}

size_t sloop_proto_sysex(char *buf, size_t cap, const uint8_t *msg, size_t n)
{
    static const char H[] = "0123456789ABCDEF";
    size_t i, o;
    int w = snprintf(buf, cap, "{\"v\":%d,\"t\":\"sysex\",\"data\":\"", SLOOP_PROTO_VERSION);
    if (w < 0 || (size_t)w + 2u * n + 3u > cap)
        return done(buf, cap, -1);
    for (o = (size_t)w, i = 0; i < n; i++) {
        buf[o++] = H[msg[i] >> 4];
        buf[o++] = H[msg[i] & 15u];
    }
    buf[o++] = '"';
    buf[o++] = '}';
    buf[o] = 0;
    return o;
}

size_t sloop_proto_midi(char *buf, size_t cap, uint32_t pkt)
{
    uint32_t cin = pkt & 0x0Fu, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
    int two = cin == 0xC || cin == 0xD;
    if (cin < 0x8 || cin > 0xE)
        return done(buf, cap, -1);
    return two ? done(buf, cap, snprintf(buf, cap, "{\"v\":%d,\"t\":\"midi\",\"data\":[%lu,%lu]}", SLOOP_PROTO_VERSION,
                                         (unsigned long)st, (unsigned long)d1))
               : done(buf, cap, snprintf(buf, cap, "{\"v\":%d,\"t\":\"midi\",\"data\":[%lu,%lu,%lu]}",
                                         SLOOP_PROTO_VERSION, (unsigned long)st, (unsigned long)d1, (unsigned long)d2));
}

void sloop_proto_rect_header(uint8_t hdr[SLOOP_PROTO_RECT_HDR], const sloop_rect_t *r)
{
    hdr[0] = SLOOP_PROTO_MSG_DISPLAY;
    hdr[1] = SLOOP_PROTO_FMT_RGB565BE;
    hdr[2] = (uint8_t)r->x; hdr[3] = (uint8_t)(r->x >> 8);
    hdr[4] = (uint8_t)r->y; hdr[5] = (uint8_t)(r->y >> 8);
    hdr[6] = (uint8_t)r->w; hdr[7] = (uint8_t)(r->w >> 8);
    hdr[8] = (uint8_t)r->h; hdr[9] = (uint8_t)(r->h >> 8);
}

/* ------------------------------------------------------------------- hub --- */
void sloop_proto_hub_init(sloop_proto_hub_t *h, const sloop_proto_ops_t *ops, const char *target, uint8_t *frame)
{
    memset(h, 0, sizeof *h);
    h->ops = *ops;
    h->target = target;
    h->frame = frame;
}

int sloop_proto_client_add(sloop_proto_hub_t *h, void *ctx)
{
    int i;
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++)
        if (!h->c[i].ctx) {
            memset(&h->c[i], 0, sizeof h->c[i]);
            h->c[i].ctx = ctx;
            h->c[i].need_hello = 1;
            h->c[i].need_full = 1;
            sloop_midi_attach(SLOOP_MIDI_WEB, 1);
            return i;
        }
    return -1;
}

static void client_drop(sloop_proto_client_t *c)
{
    unsigned i;
    for (i = 0; i < SLOOP_BTN_COUNT; i++)
        if ((c->held_btn >> i) & 1u)
            sloop_post_button(i, 0);
    for (i = 0; i < SLOOP_NKEYS; i++)
        if ((c->held_key >> i) & 1u)
            sloop_post_key(i, 0);
    c->held_btn = c->held_key = 0;
    c->ctx = NULL;
}

void sloop_proto_client_remove(sloop_proto_hub_t *h, void *ctx)
{
    int i;
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++)
        if (h->c[i].ctx && h->c[i].ctx == ctx)
            client_drop(&h->c[i]);
    if (!sloop_proto_clients(h))
        sloop_midi_attach(SLOOP_MIDI_WEB, 0);
}

int sloop_proto_clients(const sloop_proto_hub_t *h)
{
    int i, n = 0;
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++)
        n += h->c[i].ctx != NULL;
    return n;
}

static sloop_proto_client_t *client_of(sloop_proto_hub_t *h, void *ctx)
{
    int i;
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++)
        if (h->c[i].ctx && h->c[i].ctx == ctx)
            return &h->c[i];
    return NULL;
}

static int send_text(sloop_proto_hub_t *h, sloop_proto_client_t *c, const char *s, size_t n)
{
    int rc;
    if (!n)
        return 0;
    rc = h->ops.send_text(c->ctx, s, n);
    if (rc < 0)
        client_drop(c);
    return rc;
}

void sloop_proto_on_text(sloop_proto_hub_t *h, void *ctx, const char *s, size_t n)
{
    static sloop_proto_msg_t m;                  /* (the hub is used by one task at a time) */
    sloop_proto_client_t *c = client_of(h, ctx);
    char out[160];
    if (!c)
        return;
    c->rx++;
    if (sloop_proto_parse(s, n, &m)) {
        send_text(h, c, out, sloop_proto_error(out, sizeof out, m.err));
        return;
    }
    switch (m.kind) {
    case SLOOP_PROTO_HELLO:
        c->need_hello = 1;
        c->need_full = 1;
        c->led_ver = c->status_ver = 0;
        c->midi_out = m.want_midi;
        c->no_screen = m.no_screen;
        if (c->no_screen)
            c->need_full = 0;
        break;
    case SLOOP_PROTO_SYSEX:
        c->sysex = 1;
        if (sloop_proto_apply(&m))
            send_text(h, c, out, sloop_proto_error(out, sizeof out, "sysex inbox full: retry"));
        break;
    case SLOOP_PROTO_FULL:
        c->need_full = 1;
        break;
    case SLOOP_PROTO_PING:
        send_text(h, c, out, sloop_proto_pong(out, sizeof out, m.n));
        break;
    default:
        if (sloop_proto_apply(&m)) {
            send_text(h, c, out, sloop_proto_error(out, sizeof out, "input queue full"));
            break;
        }
        if (m.kind == SLOOP_PROTO_BTN)
            c->held_btn = m.down ? c->held_btn | 1u << m.id : c->held_btn & ~(1u << m.id);
        else if (m.kind == SLOOP_PROTO_KEY)
            c->held_key = m.down ? c->held_key | 1u << m.id : c->held_key & ~(1u << m.id);
        break;
    }
}

void sloop_proto_pump(sloop_proto_hub_t *h)
{
    static char txt[512];
    uint8_t led[SLOOP_LED_COUNT];
    sloop_status_t st;
    sloop_rect_t r;
    uint32_t lv;
    int i, got;
    h->pumps++;
    if (!sloop_proto_clients(h))
        return;
    /* what changed on the screen since the last pump: to every client that has the screen */
    got = sloop_display_take(&r, h->frame + SLOOP_PROTO_RECT_HDR, SLOOP_LCD_BYTES);
    if (got == 1) {
        size_t n = SLOOP_PROTO_RECT_HDR + (size_t)r.w * r.h * 2u;
        sloop_proto_rect_header(h->frame, &r);
        h->rects++;
        for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++) {
            sloop_proto_client_t *c = &h->c[i];
            int rc;
            if (!c->ctx || c->need_hello || c->need_full || c->no_screen)
                continue;
            rc = h->ops.send_bin(c->ctx, h->frame, n);
            if (rc < 0)
                client_drop(c);
            else if (rc > 0) {                     /* missed it: a full screen when it can take one */
                c->need_full = 1;
                c->dropped++;
            } else
                c->tx_rects++;
        }
    }
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++) {
        sloop_proto_client_t *c = &h->c[i];
        if (c->ctx && c->need_hello && send_text(h, c, txt, sloop_proto_hello(txt, sizeof txt, h->target)) == 0)
            c->need_hello = 0;
    }
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++) {
        sloop_proto_client_t *c = &h->c[i];
        if (c->ctx && !c->need_hello && c->need_full && !c->no_screen) {
            sloop_rect_t all = {0, 0, SLOOP_LCD_W, SLOOP_LCD_H};
            int rc;
            sloop_display_copy_full(h->frame + SLOOP_PROTO_RECT_HDR);
            sloop_proto_rect_header(h->frame, &all);
            rc = h->ops.send_bin(c->ctx, h->frame, SLOOP_PROTO_FRAME_BYTES);
            if (rc < 0)
                client_drop(c);
            else if (rc == 0) {
                c->need_full = 0;
                c->tx_full++;
            }
        }
    }
    {   /* SLOOP's SysEx replies to the editors, its MIDI out to who asked */
        static uint8_t sx[SLOOP_SYSEX_MAX + 2u];
        static char js[2u * (SLOOP_SYSEX_MAX + 2u) + 64u];
        size_t n;
        uint32_t pkt;
        while ((n = sloop_take_sysex(SLOOP_MIDI_WEB, sx, sizeof sx)) != 0) {
            size_t k = sloop_proto_sysex(js, sizeof js, sx, n);
            for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++)
                if (h->c[i].ctx && h->c[i].sysex)
                    send_text(h, &h->c[i], js, k);
        }
        while (sloop_take_midi(SLOOP_MIDI_WEB, &pkt)) {
            size_t k = sloop_proto_midi(js, sizeof js, pkt);
            for (i = 0; k && i < SLOOP_PROTO_MAX_CLIENTS; i++)
                if (h->c[i].ctx && h->c[i].midi_out)
                    send_text(h, &h->c[i], js, k);
        }
    }
    lv = sloop_leds_get(led);
    sloop_status_get(&st);
    for (i = 0; i < SLOOP_PROTO_MAX_CLIENTS; i++) {
        sloop_proto_client_t *c = &h->c[i];
        if (!c->ctx || c->need_hello || c->no_screen)
            continue;
        if (c->led_ver != lv + 1u && send_text(h, c, txt, sloop_proto_leds(txt, sizeof txt, led)) == 0)
            c->led_ver = lv + 1u;
        if (c->ctx && c->status_ver != st.version + 1u &&
            send_text(h, c, txt, sloop_proto_status(txt, sizeof txt, &st)) == 0)
            c->status_ver = st.version + 1u;
    }
}
