/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: SLOOP as a class-compliant USB-MIDI device on the ESP32-S3's native USB port,
 * as the FM-1 is on its USB-C port (upstream usb.c): MIDI in plays SLOOP, SLOOP's MIDI out (the
 * keys' notes) goes to the computer, and SLOOP's own web editor (upstream web/editor.html, Web MIDI
 * SysEx) finds the device by its name, "Felucca", and edits it over USB as on the FM-1.
 *
 * Two parts:
 *   the bridge (usb_midi_bridge.c, portable, host-tested): USB-MIDI event packets <-> SLOOP
 *       (sloop_post_midi / sloop_post_sysex in, sloop_take_sysex / sloop_take_midi out, as the
 *       SLOOP_MIDI_USB transport). A host that stops reading (no program has the port open, the
 *       computer sleeps) is waited for USB_MIDI_STALL_MS, as usb.c waits, then what SLOOP sends is
 *       dropped until it reads again: it never holds up the other transports.
 *   the device (usb_midi.c, ESP-IDF + TinyUSB): descriptors, the driver, a task polling both ways.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "sloop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIDI_STALL_MS 200u

typedef struct {
    uint32_t rx_packets, rx_sysex, rx_dropped;     /* from the host; dropped: SLOOP's inbox was full */
    uint32_t tx_packets, tx_dropped;               /* to the host; dropped: messages it did not read */
    uint32_t links;                                /* times a host configured the device */
} usb_midi_counts_t;

typedef struct {
    uint8_t up;                                    /* a host configured the device (and is awake) */
    uint8_t rx_on, rx_over;                        /* inside F0 .. F7; it got too long */
    size_t rx_n;
    uint8_t rx_sx[SLOOP_SYSEX_MAX + 2u];
    size_t tx_n, tx_pos;                           /* the SysEx message going out */
    uint8_t tx_sx[SLOOP_SYSEX_MAX + 2u];
    uint32_t tx_note;                              /* a MIDI packet waiting for room */
    uint8_t tx_note_pend;
    uint8_t stalled, deaf;                         /* no room since stall_ms; for too long */
    uint32_t stall_ms;
    usb_midi_counts_t n;
} usb_midi_bridge_t;

/* puts one 4-byte USB-MIDI event packet into the link's IN FIFO: 1, or 0 when it is full */
typedef int (*usb_midi_write_fn)(void *ctx, const uint8_t pkt[4]);

void usb_midi_bridge_init(usb_midi_bridge_t *b);
/* the link came up (a host configured the device) or went down: attaches SLOOP_MIDI_USB */
void usb_midi_bridge_link(usb_midi_bridge_t *b, int up);
/* one packet from the host (OUT endpoint) */
void usb_midi_bridge_rx(usb_midi_bridge_t *b, const uint8_t pkt[4]);
/* moves what SLOOP sent to the link, as far as it takes it (call every millisecond or so) */
void usb_midi_bridge_tx(usb_midi_bridge_t *b, uint32_t now_ms, usb_midi_write_fn write, void *ctx);

/* ---- the device (ESP32-S3, CONFIG_SLOOPY_USB_MIDI) */
typedef struct {
    int enabled;                                   /* built with SLOOPY_USB_MIDI and started */
    int linked;                                    /* a host has it configured now */
    usb_midi_counts_t n;
} usb_midi_stats_t;
int usb_midi_start(void);                          /* 0, or -1 (not built in / driver failed) */
void usb_midi_get_stats(usb_midi_stats_t *st);

#ifdef __cplusplus
}
#endif
