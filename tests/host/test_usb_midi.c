/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: the USB-MIDI device's bridge (components/usb_midi) against the SLOOP
 * core, with a simulated USB link (the host's packets in, an IN FIFO that can stop taking):
 *   a note from the computer plays; SLOOP's editor answers SysEx sent as USB-MIDI packets, in
 *   correctly framed packets; the web panel and USB both get every reply and every MIDI-out note;
 *   a host that stops reading is dropped after USB_MIDI_STALL_MS and never holds up the web
 *   editor; it is served again when it reads; nothing reaches USB while it is unplugged; a
 *   re-plugged host gets nothing stale; malformed SysEx is ignored.
 *   test_usb_midi [OUTDIR]                                                                        */
#include "sim.h"
#include "usb_midi.h"

static usb_midi_bridge_t br;

/* ---- the link: what the device sent to the host (IN endpoint), and whether it takes more */
static struct {
    int open;                         /* the host reads the IN endpoint */
    uint8_t pkt[4096][4];
    unsigned n;
} in;

static int link_write(void *ctx, const uint8_t pkt[4])
{
    (void)ctx;
    if (!in.open || in.n >= 4096u)
        return 0;
    memcpy(in.pkt[in.n++], pkt, 4);
    return 1;
}

/* run ms milliseconds of SLOOP, the bridge polled every millisecond (as the device task does) */
static void run(uint32_t ms)
{
    while (ms--) {
        sim_run_ms(1);
        usb_midi_bridge_tx(&br, (uint32_t)(sim.t_us / 1000u), link_write, NULL);
    }
}

/* the host sends a whole SysEx message as USB-MIDI packets (cable 0) */
static void host_sysex(const uint8_t *m, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t k = n - i >= 3u ? 3u : n - i;
        uint8_t p[4] = {(uint8_t)(k == 3u && i + 3u < n ? 4u : 4u + k), m[i], k > 1u ? m[i + 1] : 0u,
                        k > 2u ? m[i + 2] : 0u};
        usb_midi_bridge_rx(&br, p);
        i += k;
    }
}
static void host_editor(uint8_t cmd)
{
    const uint8_t m[6] = {0xF0, 0x7D, 0x46, 0x4C, cmd, 0xF7};
    host_sysex(m, sizeof m);
}

/* what the host received: SysEx messages reassembled (framing checked), MIDI packets counted */
static struct {
    uint8_t msg[64][700];
    size_t len[64];
    unsigned n, bad_framing, notes_on, notes_off;
    uint8_t last_note_status, last_note;
} got;

static void host_parse(void)
{
    static uint8_t cur[700];
    static size_t cn;
    unsigned i;
    memset(&got, 0, sizeof got);
    cn = 0;
    for (i = 0; i < in.n; i++) {
        const uint8_t *p = in.pkt[i];
        uint32_t cin = p[0] & 0x0Fu, k, j;
        if (p[0] >> 4)
            got.bad_framing++;                     /* (cable 0 only) */
        if (cin >= 4u && cin <= 7u) {
            k = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
            for (j = 0; j < k; j++) {
                if (p[1 + j] == 0xF0u)
                    cn = 0;
                if (cn < sizeof cur)
                    cur[cn++] = p[1 + j];
            }
            if (cin == 4u && (p[1] == 0xF7u || p[2] == 0xF7u || p[3] == 0xF7u))
                got.bad_framing++;                 /* an F7 must end the message */
            if (cin >= 5u) {
                if (cur[0] != 0xF0u || cur[cn - 1u] != 0xF7u)
                    got.bad_framing++;
                else if (got.n < 64u) {
                    memcpy(got.msg[got.n], cur, cn);
                    got.len[got.n++] = cn;
                }
                cn = 0;
            }
        } else if (cin == 9u && p[3]) {
            got.notes_on++;
            got.last_note_status = p[1];
            got.last_note = p[2];
        } else if (cin == 8u || cin == 9u) {
            got.notes_off++;
        }
    }
}

static int has(const uint8_t *m, size_t n, const char *s)
{
    size_t i, k = strlen(s);
    for (i = 0; i + k <= n; i++)
        if (!memcmp(m + i, s, k))
            return 1;
    return 0;
}

static int is_reply(unsigned i, uint8_t cmd)
{
    return got.len[i] >= 6u && got.msg[i][0] == 0xF0u && got.msg[i][1] == 0x7Du && got.msg[i][2] == 0x46u &&
           got.msg[i][3] == 0x4Cu && got.msg[i][4] == cmd;
}

static unsigned web_replies(uint8_t cmd)
{
    static uint8_t m[SLOOP_SYSEX_MAX + 2u];
    unsigned n = 0;
    size_t k;
    while ((k = sloop_take_sysex(SLOOP_MIDI_WEB, m, sizeof m)) != 0)
        n += k >= 6u && m[4] == cmd;
    return n;
}

int main(int argc, char **argv)
{
    char store[512];
    unsigned i, n;
    uint32_t pkt;
    snprintf(store, sizeof store, "%s/usb_midi.store", argc > 1 ? argv[1] : "build-host/out");
    remove(store);
    printf("test_usb_midi: SLOOP as a USB-MIDI device (bridge + core, simulated link)\n");
    sim_boot(store);
    sim_run_ms(1500);
    usb_midi_bridge_init(&br);
    in.open = 1;

    /* nothing before a host configures the device */
    usb_midi_bridge_rx(&br, (const uint8_t[4]){0x09, 0x90, 60, 100});
    run(50);
    sim_check(br.n.rx_packets == 0 && in.n == 0, "unplugged: packets are ignored, nothing is sent");

    usb_midi_bridge_link(&br, 1);
    sim_check(br.up && br.n.links == 1, "a host configured the device: the link is up");

    /* MIDI in: a note from the computer plays (cable 1 too: the cable number is ignored) */
    usb_midi_bridge_rx(&br, (const uint8_t[4]){0x19, 0x90, 60, 100});
    sim.peak = 0;
    run(500);
    usb_midi_bridge_rx(&br, (const uint8_t[4]){0x08, 0x80, 60, 0});
    sim_check(sim.peak > 3000, "MIDI in over USB: a note on channel 1 is heard");
    run(1500);
    sim.peak = 0;
    run(1000);
    sim_check(sim.peak < 3000, "MIDI in over USB: its note off ends it");
    usb_midi_bridge_rx(&br, (const uint8_t[4]){0x09, 0x80, 60, 100});   /* CIN does not match the status */
    usb_midi_bridge_rx(&br, (const uint8_t[4]){0x0F, 0xF8, 0, 0});      /* clock: ignored, as upstream */
    run(200);
    sim_check(sim_peak_over(200) < 3000, "MIDI in: inconsistent packets and real time are ignored");

    /* SLOOP's editor over USB: INFO split across packets, the reply framed as USB-MIDI SysEx */
    in.n = 0;
    host_editor(1);
    run(100);
    host_parse();
    sim_check(got.n == 1 && is_reply(0, 1) && !got.bad_framing, "editor over USB: INFO gets one well-framed reply");
    sim_check(got.n == 1 && got.len[0] > 20u && got.msg[0][got.len[0] - 2u] == 5u,
              "editor over USB: INFO ends with protocol version 5");
    sim_check(got.n == 1 && has(got.msg[0], got.len[0], "SLOOP"), "editor over USB: it is SLOOP");
    sim_check(br.n.rx_sysex == 1, "editor over USB: one request assembled from the packets");

    /* the web panel attached too: both transports get every reply and every MIDI-out note */
    sloop_midi_attach(SLOOP_MIDI_WEB, 1);
    in.n = 0;
    host_editor(25);                               /* PING, from USB */
    run(50);
    {
        const uint8_t ping[6] = {0xF0, 0x7D, 0x46, 0x4C, 25, 0xF7};
        sloop_post_sysex(ping, sizeof ping);       /* PING, from the web */
    }
    run(50);
    host_parse();
    sim_check(got.n == 2 && is_reply(0, 25) && is_reply(1, 25), "fan-out: USB gets both PING replies");
    sim_check(web_replies(25) == 2, "fan-out: the web gets both PING replies");
    in.n = 0;
    sim_key(0, 80, 100);                           /* F3 on the panel: SLOOP plays it and sends MIDI out */
    run(20);
    host_parse();
    n = 0;
    while (sloop_take_midi(SLOOP_MIDI_WEB, &pkt))
        n += ((pkt >> 8) & 0xF0u) == 0x90u && (pkt >> 24) != 0u;
    sim_check(got.notes_on == 1 && (got.last_note_status & 0xF0u) == 0x90u && got.notes_off == 1,
              "fan-out: a key's note on / off reaches USB");
    sim_check(n == 1, "fan-out: and the web");

    /* the host stops reading (no program has the port open): after USB_MIDI_STALL_MS SLOOP's
     * output to USB is dropped; the web editor keeps getting every reply */
    in.open = 0;
    n = 0;
    for (i = 0; i < 150; i++) {
        const uint8_t ping[6] = {0xF0, 0x7D, 0x46, 0x4C, 25, 0xF7};
        sloop_post_sysex(ping, sizeof ping);
        run(20);
        n += web_replies(25);
    }
    sim_check(br.deaf && br.n.tx_dropped > 0, "stalled host: dropped after the stall time");
    sim_check(n == 150, "stalled host: the web editor got all 150 replies (never held up)");
    printf("    (USB dropped %lu messages)\n", (unsigned long)br.n.tx_dropped);
    sim_key(2, 80, 100);
    run(20);
    n = 0;
    while (sloop_take_midi(SLOOP_MIDI_WEB, &pkt))
        n++;
    sim_check(n == 2, "stalled host: the web still gets MIDI-out notes");

    /* it reads again: served again, nothing is half a message */
    in.open = 1;
    in.n = 0;
    host_editor(25);
    run(50);
    host_parse();
    sim_check(!br.deaf && got.n >= 1 && is_reply(got.n - 1u, 25) && !got.bad_framing,
              "the host reads again: replies reach it again, well framed");

    /* unplugged: SLOOP_MIDI_USB is detached, nothing waits for it */
    usb_midi_bridge_link(&br, 0);
    in.n = 0;
    sim_key(4, 80, 100);
    run(20);
    sim_check(!sloop_take_midi(SLOOP_MIDI_USB, &pkt) && in.n == 0, "unplugged: nothing for USB");
    n = 0;
    while (sloop_take_midi(SLOOP_MIDI_WEB, &pkt))
        n++;
    sim_check(n == 2, "unplugged: the web still gets the notes");

    /* plugged in again: nothing stale from while it was away */
    usb_midi_bridge_link(&br, 1);
    run(50);
    sim_check(in.n == 0 && br.n.links == 2, "re-plugged: nothing stale is sent");
    sloop_midi_attach(SLOOP_MIDI_WEB, 0);

    /* malformed SysEx: a status byte inside, one longer than SLOOP's frame, F7 alone */
    {
        static uint8_t big[SLOOP_SYSEX_MAX + 16u];
        const uint8_t broken[7] = {0xF0, 0x7D, 0x46, 0x90, 0x4C, 1, 0xF7};
        uint32_t before = br.n.rx_dropped, sysex = br.n.rx_sysex;
        in.n = 0;
        host_sysex(broken, sizeof broken);
        memset(big, 0x11, sizeof big);
        big[0] = 0xF0;
        big[sizeof big - 1u] = 0xF7;
        host_sysex(big, sizeof big);
        usb_midi_bridge_rx(&br, (const uint8_t[4]){0x05, 0xF7, 0, 0});
        run(100);
        sim_check(br.n.rx_dropped == before + 1u && br.n.rx_sysex == sysex && in.n == 0,
                  "malformed SysEx: ignored (the too-long one counted as dropped)");
        host_editor(25);
        run(50);
        host_parse();
        sim_check(got.n == 1 && is_reply(0, 25), "malformed SysEx: the next request is answered");
    }

    usb_midi_bridge_link(&br, 0);
    printf("test_usb_midi: %s (%d failure%s)\n", sim_fails ? "FAIL" : "PASS", sim_fails, sim_fails == 1 ? "" : "s");
    return sim_fails ? 1 : 0;
}
