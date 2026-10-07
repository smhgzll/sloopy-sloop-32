/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: the web panel protocol (components/sloop_proto) against the real
 * core: what the parser accepts and rejects, the encoders, and the hub with a fake transport
 * (hello, full screen, rectangles, LEDs, status, pong, errors, a busy client). */
#include <string.h>

#include "sim.h"
#include "sloop_proto.h"

static int parse_ok(const char *s, sloop_proto_msg_t *m) { return sloop_proto_parse(s, strlen(s), m) == 0; }

static void test_parser(void)
{
    sloop_proto_msg_t m;
    sim_check(parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":\"PLAY\",\"down\":true}", &m) && m.kind == SLOOP_PROTO_BTN &&
                  m.id == SLOOP_BTN_PLAY && m.down == 1, "parse: btn PLAY down");
    sim_check(parse_ok(" { \"t\" : \"btn\" , \"down\" : false , \"id\" : \"OCT-\" , \"v\" : 1 } ", &m) &&
                  m.id == SLOOP_BTN_OCTDN && m.down == 0, "parse: any key order, spaces, OCT-");
    sim_check(parse_ok("{\"v\":1,\"t\":\"key\",\"k\":26,\"down\":true}", &m) && m.kind == SLOOP_PROTO_KEY && m.id == 26,
              "parse: key 26 (G5)");
    sim_check(parse_ok("{\"v\":1,\"t\":\"enc\",\"id\":\"K4\",\"d\":-3}", &m) && m.kind == SLOOP_PROTO_ENC &&
                  m.id == SLOOP_ENC_K4 && m.value == -3, "parse: enc K4 -3");
    sim_check(parse_ok("{\"v\":1,\"t\":\"enc\",\"id\":\"ALGORITHM\",\"d\":1}", &m) && m.id == SLOOP_ENC_ALGO,
              "parse: enc ALGORITHM");
    sim_check(parse_ok("{\"v\":1,\"t\":\"pot\",\"id\":\"MASTER\",\"val\":1023}", &m) && m.kind == SLOOP_PROTO_POT &&
                  m.value == 1023, "parse: pot MASTER 1023");
    sim_check(parse_ok("{\"v\":1,\"t\":\"midi\",\"data\":[144,60,100]}", &m) && m.kind == SLOOP_PROTO_MIDI &&
                  m.midi_len == 3 && m.midi[1] == 60, "parse: midi note on");
    sim_check(parse_ok("{\"v\":1,\"t\":\"midi\",\"data\":[192,5]}", &m) && m.midi_len == 2, "parse: midi program change");
    sim_check(parse_ok("{\"v\":1,\"t\":\"ping\",\"n\":42,\"extra\":[1,2],\"note\":\"x\\\"y\",\"z\":null}", &m) &&
                  m.kind == SLOOP_PROTO_PING && m.n == 42, "parse: ping, unknown keys skipped");
    sim_check(parse_ok("{\"v\":1,\"t\":\"hello\"}", &m) && m.kind == SLOOP_PROTO_HELLO, "parse: hello");

    sim_check(!parse_ok("{\"t\":\"btn\",\"id\":\"PLAY\",\"down\":true}", &m), "reject: no version");
    sim_check(!parse_ok("{\"v\":2,\"t\":\"btn\",\"id\":\"PLAY\",\"down\":true}", &m), "reject: version 2");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":\"PLAYX\",\"down\":true}", &m), "reject: unknown button");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":\"PLAY\"}", &m), "reject: btn without down");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"key\",\"k\":27,\"down\":true}", &m), "reject: key 27");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"enc\",\"id\":\"K1\",\"d\":0}", &m), "reject: enc d = 0");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"enc\",\"id\":\"K1\",\"d\":65}", &m), "reject: enc d = 65");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"pot\",\"id\":\"MASTER\",\"val\":1.5}", &m), "reject: fractions");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"midi\",\"data\":[240,1,2]}", &m), "reject: midi system message");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"midi\",\"data\":[144,200,1]}", &m), "reject: midi data byte > 127");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":\"PLAY\",\"down\":true} x", &m), "reject: trailing bytes");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":{\"a\":1},\"down\":true}", &m), "reject: nested objects");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"btn\",\"id\":\"PLA", &m), "reject: truncated");
    sim_check(!parse_ok("", &m) && m.err[0], "reject: empty, with a reason");
    sim_check(parse_ok("{\"v\":1,\"t\":\"sysex\",\"data\":\"F07D464C01F7\"}", &m) && m.kind == SLOOP_PROTO_SYSEX &&
                  m.sx_len == 6 && m.sx[4] == 1, "parse: sysex (hex)");
    sim_check(parse_ok("{\"v\":1,\"t\":\"sysex\",\"data\":\"f07d464c19f7\"}", &m) && m.sx[4] == 0x19, "parse: sysex, lower case");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"sysex\",\"data\":\"F07D464\"}", &m), "reject: sysex, odd hex");
    sim_check(!parse_ok("{\"v\":1,\"t\":\"sysex\",\"data\":\"907D46\"}", &m), "reject: sysex not F0..F7");
    sim_check(parse_ok("{\"v\":1,\"t\":\"hello\",\"screen\":false,\"midi\":true}", &m) && m.no_screen && m.want_midi,
              "parse: hello options (no screen, MIDI out)");
    {
        char big[2 * (SLOOP_SYSEX_MAX + 10) + 64];
        int n = snprintf(big, sizeof big, "{\"v\":1,\"t\":\"sysex\",\"data\":\"F0");
        unsigned i;
        for (i = 0; i < SLOOP_SYSEX_MAX + 4u; i++)
            n += snprintf(big + n, sizeof big - (size_t)n, "01");
        snprintf(big + n, sizeof big - (size_t)n, "F7\"}");
        sim_check(!parse_ok(big, &m), "reject: sysex longer than SLOOP's frame");
    }
    {   /* every prefix of a valid message is rejected, never crashes */
        const char *s = "{\"v\":1,\"t\":\"enc\",\"id\":\"K1\",\"d\":-12}";
        size_t n, bad = 0;
        for (n = 0; n < strlen(s); n++)
            bad += sloop_proto_parse(s, n, &m) != 0;
        sim_check(bad == strlen(s), "reject: every truncation of a valid message");
    }
}

/* ---- a fake transport */
typedef struct {
    int busy;
    uint32_t texts, bins, full, rects;
    char last_text[1024];
    char seen[64][16];               /* "t" of each text message, in order */
    uint32_t nseen;
    sloop_rect_t last_rect;
} fake_t;

static int f_text(void *ctx, const char *s, size_t n)
{
    fake_t *f = ctx;
    const char *t;
    if (f->busy)
        return 1;
    f->texts++;
    n = n < sizeof f->last_text - 1 ? n : sizeof f->last_text - 1;
    memcpy(f->last_text, s, n);
    f->last_text[n] = 0;
    t = strstr(f->last_text, "\"t\":\"");
    if (t && f->nseen < 64)
        sscanf(t + 5, "%15[a-z]", f->seen[f->nseen++]);
    return 0;
}

static int f_bin(void *ctx, const uint8_t *p, size_t n)
{
    fake_t *f = ctx;
    if (f->busy)
        return 1;
    f->bins++;
    f->last_rect.x = (uint16_t)(p[2] | p[3] << 8);
    f->last_rect.y = (uint16_t)(p[4] | p[5] << 8);
    f->last_rect.w = (uint16_t)(p[6] | p[7] << 8);
    f->last_rect.h = (uint16_t)(p[8] | p[9] << 8);
    if (p[0] == SLOOP_PROTO_MSG_DISPLAY && n == SLOOP_PROTO_RECT_HDR + (size_t)f->last_rect.w * f->last_rect.h * 2u) {
        if (f->last_rect.w == SLOOP_LCD_W && f->last_rect.h == SLOOP_LCD_H)
            f->full++;
        else
            f->rects++;
    }
    return 0;
}

static int seen(const fake_t *f, const char *t)
{
    uint32_t i;
    for (i = 0; i < f->nseen; i++)
        if (!strcmp(f->seen[i], t))
            return 1;
    return 0;
}

static void test_hub(void)
{
    static uint8_t frame[SLOOP_PROTO_FRAME_BYTES];
    static sloop_proto_hub_t hub;
    sloop_proto_ops_t ops = {f_text, f_bin};
    fake_t a, b;
    const char *play_dn = "{\"v\":1,\"t\":\"btn\",\"id\":\"PLAY\",\"down\":true}";
    const char *play_up = "{\"v\":1,\"t\":\"btn\",\"id\":\"PLAY\",\"down\":false}";
    sloop_status_t st;
    uint32_t rects0;

    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    sim_boot(NULL);
    sim_run_ms(1500);
    sloop_proto_hub_init(&hub, &ops, "host", frame);
    sim_check(sloop_proto_client_add(&hub, &a) >= 0, "hub: client added");
    sloop_proto_pump(&hub);
    sim_check(a.nseen >= 3 && !strcmp(a.seen[0], "hello") && seen(&a, "leds") && seen(&a, "status"),
              "hub: hello first, then LEDs and status");
    sim_check(a.full == 1, "hub: a new client gets the whole screen");
    sim_check(strstr(a.seen[0], "hello") && strstr(a.last_text, "\"t\":\"status\""), "hub: status JSON");

    sloop_proto_on_text(&hub, &a, play_dn, strlen(play_dn));
    sim_run_ms(60);
    sloop_proto_on_text(&hub, &a, play_up, strlen(play_up));
    sim_run_ms(300);
    sloop_status_get(&st);
    sim_check(st.playing == 1, "hub: a btn message from the browser starts the transport");
    rects0 = a.rects;
    sloop_proto_pump(&hub);
    sim_check(strstr(a.last_text, "\"playing\":1") != NULL, "hub: status pushed after the change");
    sim_check(a.rects == rects0 + 1, "hub: the screen change goes out as one rectangle");
    sim_check(a.last_rect.w > 0 && a.last_rect.w < SLOOP_LCD_W * 2u, "hub: rectangle inside the screen");

    {
        const char *ping = "{\"v\":1,\"t\":\"ping\",\"n\":7}", *bad = "{\"v\":1,\"t\":\"nope\"}";
        sloop_proto_on_text(&hub, &a, ping, strlen(ping));
        sim_check(strstr(a.last_text, "\"t\":\"pong\",\"n\":7") != NULL, "hub: ping -> pong");
        sloop_proto_on_text(&hub, &a, bad, strlen(bad));
        sim_check(strstr(a.last_text, "\"t\":\"err\"") != NULL, "hub: a bad message -> err");
    }

    /* a second client joins later; a busy client misses a rectangle and gets a full screen */
    sloop_proto_client_add(&hub, &b);
    sloop_proto_pump(&hub);
    sim_check(b.full == 1 && seen(&b, "hello"), "hub: second client gets hello + screen");
    a.busy = 1;
    sloop_post_encoder(SLOOP_ENC_K1, 3);
    sim_run_ms(100);
    sloop_proto_pump(&hub);
    a.busy = 0;
    sloop_proto_pump(&hub);
    sim_check(a.full == 2, "hub: a busy client gets a full screen after missing an update");
    sloop_proto_client_remove(&hub, &a);
    sloop_proto_client_remove(&hub, &b);
    sim_check(sloop_proto_clients(&hub) == 0, "hub: clients removed");

    /* a client that goes away while holding controls leaves nothing stuck down */
    {
        const char *fx = "{\"v\":1,\"t\":\"btn\",\"id\":\"FX\",\"down\":true}";
        const char *key = "{\"v\":1,\"t\":\"key\",\"k\":3,\"down\":true}";
        uint8_t led[SLOOP_LED_COUNT];
        sloop_proto_client_add(&hub, &a);
        sloop_proto_on_text(&hub, &a, fx, strlen(fx));
        sloop_proto_on_text(&hub, &a, key, strlen(key));
        sim_run_ms(200);
        sloop_leds_get(led);
        sim_check(led[SLOOP_BTN_FX] == 2, "hub: FX held by a client: its layer is lit");
        sim_check(sloop_proto_clients(&hub) == 1, "hub: the client holds FX and a key");
        sloop_proto_client_remove(&hub, &a);
        sim_run_ms(200);
        sloop_leds_get(led);
        sim_check(led[SLOOP_BTN_FX] != 2 && led[SLOOP_BTN_COUNT + 3] != 2,
                  "hub: disconnect releases FX and the key it held");
        sloop_proto_client_add(&hub, &b);
        sloop_post_key(5, 1);                    /* (another source still works normally) */
        sim_run_ms(100);
        sloop_post_key(5, 0);
        sim_run_ms(100);
        sloop_leds_get(led);
        sim_check(led[SLOOP_BTN_COUNT + 5] != 2, "hub: keys work normally after the release");
        sloop_proto_client_remove(&hub, &b);
    }
}

static void test_encoders(void)
{
    char buf[1024];
    uint8_t led[SLOOP_LED_COUNT] = {0};
    size_t n;
    led[SLOOP_BTN_PLAY] = 2;
    led[SLOOP_BTN_COUNT + 4] = 1;
    n = sloop_proto_leds(buf, sizeof buf, led);
    sim_check(n > 0 && strstr(buf, "\"s\":\"0000000000200000001") != NULL, "encode: leds string (PLAY lit, key 5 dim)");
    n = sloop_proto_hello(buf, sizeof buf, "host");
    sim_check(n > 0 && strstr(buf, "\"buttons\":[\"FX\",") && strstr(buf, "\"OCT+\"]") && strstr(buf, "\"K4\"]"),
              "encode: hello lists the controls");
    sim_check(sloop_proto_hello(buf, 40, "host") == 0 && buf[0] == 0, "encode: too small a buffer -> 0, empty");
    {
        const uint8_t sx[] = {0xF0, 0x7D, 0x46, 0x4C, 0x19, 0x00, 0xF7};
        n = sloop_proto_sysex(buf, sizeof buf, sx, sizeof sx);
        sim_check(n > 0 && strstr(buf, "\"data\":\"F07D464C1900F7\"") != NULL, "encode: sysex as hex");
        n = sloop_proto_midi(buf, sizeof buf, 0x09u | 0x90u << 8 | 60u << 16 | 100u << 24);
        sim_check(n > 0 && strstr(buf, "\"data\":[144,60,100]") != NULL, "encode: MIDI out note on");
        n = sloop_proto_midi(buf, sizeof buf, 0x0Cu | 0xC2u << 8 | 7u << 16);
        sim_check(n > 0 && strstr(buf, "\"data\":[194,7]") != NULL, "encode: MIDI out program change (2 bytes)");
    }
}

int main(void)
{
    printf("proto:\n");
    test_parser();
    test_encoders();
    test_hub();
    printf("proto: %s\n", sim_fails ? "FAIL" : "PASS");
    return sim_fails ? 1 : 0;
}
