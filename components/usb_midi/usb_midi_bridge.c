/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: USB-MIDI event packets <-> SLOOP (see usb_midi.h). Portable: the host tests
 * run it against the SLOOP core with a simulated link.
 *
 * In, as upstream usb.c's ep1_rx: channel messages (CIN 8..E) go to SLOOP's MIDI ring, SysEx
 * (CIN 4..7) is assembled into whole F0..F7 messages for SLOOP's editor; the cable number and
 * anything else (system common / real time) are ignored. Out: SysEx first (as usb.c sends its
 * SysEx ring before any MIDI), then MIDI, packed as usb.c's ota_wire_send packs SysEx.
 */
#include "usb_midi.h"

#include <string.h>

void usb_midi_bridge_init(usb_midi_bridge_t *b) { memset(b, 0, sizeof *b); }

void usb_midi_bridge_link(usb_midi_bridge_t *b, int up)
{
    usb_midi_counts_t n = b->n;
    up = up ? 1 : 0;
    if (up == b->up)
        return;
    memset(b, 0, sizeof *b);                        /* nothing half-received or half-sent survives */
    b->n = n;
    b->up = (uint8_t)up;
    if (up)
        b->n.links++;
    sloop_midi_attach(SLOOP_MIDI_USB, up);
}

static void rx_sysex_byte(usb_midi_bridge_t *b, uint8_t c)
{
    if (c == 0xF0u) {
        b->rx_on = 1;
        b->rx_over = 0;
        b->rx_n = 0;
    } else if (!b->rx_on) {
        return;
    } else if (c & 0x80u && c != 0xF7u) {
        b->rx_on = 0;                               /* a status byte inside: not a SysEx after all */
        return;
    }
    if (b->rx_n < sizeof b->rx_sx)
        b->rx_sx[b->rx_n++] = c;
    else
        b->rx_over = 1;
    if (c == 0xF7u) {
        b->rx_on = 0;
        if (b->rx_over || sloop_post_sysex(b->rx_sx, b->rx_n) != 0)
            b->n.rx_dropped++;                      /* (too long, or the inbox is full: the editor retries) */
        else
            b->n.rx_sysex++;
    }
}

void usb_midi_bridge_rx(usb_midi_bridge_t *b, const uint8_t pkt[4])
{
    uint32_t cin = pkt[0] & 0x0Fu, i, k;
    if (!b->up)
        return;
    b->n.rx_packets++;
    if (cin >= 4u && cin <= 7u) {
        k = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
        for (i = 0; i < k; i++)
            rx_sysex_byte(b, pkt[1 + i]);
    } else if (cin >= 8u && cin <= 0xEu && pkt[1] >> 4 == cin) {
        if (sloop_post_midi(cin | (uint32_t)pkt[1] << 8 | (uint32_t)(pkt[2] & 0x7Fu) << 16 |
                            (uint32_t)(pkt[3] & 0x7Fu) << 24) != 0)
            b->n.rx_dropped++;
    }
}

/* one packet to the link; tracks how long the link has had no room */
static int put(usb_midi_bridge_t *b, uint32_t now_ms, const uint8_t pkt[4], usb_midi_write_fn write, void *ctx)
{
    if (write(ctx, pkt)) {
        b->stalled = b->deaf = 0;
        b->n.tx_packets++;
        return 1;
    }
    if (!b->stalled) {
        b->stalled = 1;
        b->stall_ms = now_ms;
    } else if (now_ms - b->stall_ms >= USB_MIDI_STALL_MS) {
        b->deaf = 1;                                /* the host is not reading: drop, do not wait */
    }
    return 0;
}

void usb_midi_bridge_tx(usb_midi_bridge_t *b, uint32_t now_ms, usb_midi_write_fn write, void *ctx)
{
    uint8_t pkt[4];
    if (!b->up)
        return;
    for (;;) {                                      /* SysEx: the editor's replies and pushes */
        size_t k;
        if (b->tx_pos >= b->tx_n) {
            b->tx_pos = 0;
            b->tx_n = sloop_take_sysex(SLOOP_MIDI_USB, b->tx_sx, sizeof b->tx_sx);
            if (!b->tx_n)
                break;
        }
        k = b->tx_n - b->tx_pos >= 3u ? 3u : b->tx_n - b->tx_pos;
        pkt[0] = (uint8_t)(k == 3u && b->tx_pos + 3u < b->tx_n ? 4u : 4u + k);   /* 4 continues; 5/6/7 end */
        pkt[1] = b->tx_sx[b->tx_pos];
        pkt[2] = k > 1u ? b->tx_sx[b->tx_pos + 1u] : 0u;
        pkt[3] = k > 2u ? b->tx_sx[b->tx_pos + 2u] : 0u;
        if (put(b, now_ms, pkt, write, ctx)) {
            b->tx_pos += k;
        } else if (b->deaf) {
            b->tx_pos = b->tx_n;                    /* (a host that comes back resyncs on the next F0) */
            b->n.tx_dropped++;
        } else {
            return;                                 /* wait for room, in order */
        }
    }
    for (;;) {                                      /* MIDI out: the notes SLOOP's keys play */
        if (!b->tx_note_pend) {
            if (!sloop_take_midi(SLOOP_MIDI_USB, &b->tx_note))
                break;
            b->tx_note_pend = 1;
        }
        pkt[0] = (uint8_t)(b->tx_note & 0x0Fu);     /* cable 0 */
        pkt[1] = (uint8_t)(b->tx_note >> 8);
        pkt[2] = (uint8_t)(b->tx_note >> 16);
        pkt[3] = (uint8_t)(b->tx_note >> 24);
        if (put(b, now_ms, pkt, write, ctx)) {
            b->tx_note_pend = 0;
        } else if (b->deaf) {
            b->tx_note_pend = 0;
            b->n.tx_dropped++;
        } else {
            return;
        }
    }
}
