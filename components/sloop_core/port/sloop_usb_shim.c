/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: stands in for upstream usb.c (the FM-1's register-level USB-MIDI device).
 *
 * SLOOP talks MIDI through usb.c's rings: seq.c drains midi_in_q in the audio ISR and fills
 * midi_out_q through midi_out_event(); the web editor (editor.c) receives SysEx frames through
 * ota_frame_get() / ota_frame_done() and replies through ota_wire_send(). This file keeps those
 * rings and entry points with the same contracts, without the USB controller: the port's
 * transports (the WebSocket hub, the USB-MIDI device) feed and drain them through sloop_midi_*
 * (port runtime). usb.config means "a MIDI transport is attached", as on the FM-1 where it means
 * "a USB host configured the device".
 *
 * Both output rings have one reader per attached transport (SLOOP_MIDI_*): each gets every
 * packet, at its own pace. A writer sees the room left by the slowest attached reader, so a ring
 * never overwrites what a transport has not read yet; editor.c's own room check (so_w - so_r)
 * reads that same slowest reader through the so_r macro below.
 *
 * Included by sloop_unity.c in place of usb.c (same position).
 */
static struct {
    uint8_t up, config, pend_addr, has_pend_addr, e0_tx, e0_zlp;
    uint32_t resets, setups, rx_pkts, tx_pkts, sof_seen, timeouts, no_sof;
    volatile uint8_t suspended;
    volatile uint8_t uboot_req;
    volatile uint8_t ota_req;
} usb;

/* the attached transports (bit per SLOOP_MIDI_*) and their read positions in the output rings */
static volatile uint8_t port_midi_on;
static volatile uint32_t port_mo_r[SLOOP_MIDI_PORTS], port_so_r[SLOOP_MIDI_PORTS];

static uint32_t port_unread(const volatile uint32_t *r, uint32_t w)   /* the slowest attached reader's */
{
    uint32_t p, most = 0, on = port_midi_on;
    for (p = 0; p < SLOOP_MIDI_PORTS; p++)
        if ((on >> p) & 1u && w - r[p] > most)
            most = w - r[p];
    return most;
}

/* MIDI rings: 4-byte USB-MIDI event packets (cable 0), as usb.c */
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_out_q[MQ];
static volatile uint32_t mi_w, mi_r, mo_w;

static void midi_out_event(uint32_t pkt)            /* from the audio ISR */
{
    if (usb.config && port_unread(port_mo_r, mo_w) < MQ) {
        midi_out_q[mo_w % MQ] = pkt;
        RING_PUBLISH();
        mo_w++;
    }
}

/* SysEx frames for the main loop (editor.c), as usb.c with FELUCCA_OTA */
static uint8_t sx_frame[640];
static volatile uint32_t sx_frame_len;
static volatile uint8_t sx_ready;
#define SXQ 64u
static uint32_t sx_out_q[SXQ];
static volatile uint32_t so_w;
#define so_r (so_w - port_unread(port_so_r, so_w))     /* (editor.c's ed_room) */

static int ota_wire_send(const uint8_t *p, uint32_t n)   /* F0..F7 -> USB-MIDI SysEx packets */
{
    uint32_t i = 0, need = (n + 2u) / 3u;
    if (!usb.config || SXQ - (so_w - so_r) < need)
        return -1;                                  /* (usb.c waits up to 200 ms for room; the port's
                                                     * transport drains every UI step, editor.c retries) */
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, pkt;
        uint32_t cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;   /* 4 continues; 5/6/7 end */
        pkt = cin | (uint32_t)p[i] << 8 | (k > 1u ? (uint32_t)p[i + 1] << 16 : 0u) |
              (k > 2u ? (uint32_t)p[i + 2] << 24 : 0u);
        sx_out_q[so_w % SXQ] = pkt;
        RING_PUBLISH();
        so_w++;
        i += k;
    }
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!sx_ready)
        return 0;
    RING_PUBLISH();
    *p = sx_frame;
    *n = sx_frame_len;
    return 1;
}
static void ota_frame_done(void)
{
    RING_PUBLISH();
    sx_ready = 0;
}

static void usb_start(void) { usb.up = 1; }
static void usb_retry(uint32_t now_ms) { (void)now_ms; }
static void usb_detach(void) {}
