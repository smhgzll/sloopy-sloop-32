/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the web panel protocol (docs/PROTOCOL.md), transport-independent.
 *
 * The same code runs behind the host app's WebSocket server and the ESP32's esp_http_server:
 * a transport registers its clients in a hub, hands it every text message a client sends, and
 * calls sloop_proto_pump() regularly (~30 Hz); the hub answers through the transport's send
 * callbacks. Plain C, no allocation, no ESP-IDF / POSIX.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "sloop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLOOP_PROTO_VERSION 1
#define SLOOP_PROTO_MAX_CLIENTS 8
#define SLOOP_PROTO_RECT_HDR 10u          /* binary display message header bytes */
#define SLOOP_PROTO_MSG_DISPLAY 0x01u     /* binary message types */
#define SLOOP_PROTO_FMT_RGB565BE 0x00u
#define SLOOP_PROTO_FRAME_BYTES (SLOOP_PROTO_RECT_HDR + SLOOP_LCD_BYTES)

/* ---- one parsed client message */
typedef enum {
    SLOOP_PROTO_NONE, SLOOP_PROTO_HELLO, SLOOP_PROTO_BTN, SLOOP_PROTO_KEY, SLOOP_PROTO_ENC,
    SLOOP_PROTO_POT, SLOOP_PROTO_MIDI, SLOOP_PROTO_PING, SLOOP_PROTO_FULL, SLOOP_PROTO_SYSEX
} sloop_proto_kind_t;
#define SLOOP_PROTO_TEXT_MAX 2048u        /* the longest client message (a SysEx frame in hex) */

typedef struct {
    sloop_proto_kind_t kind;
    int id;                               /* btn / enc / pot index, key number */
    int down;                             /* btn / key */
    int value;                            /* enc steps, pot value */
    uint32_t n;                           /* ping id */
    uint8_t midi[3], midi_len;
    uint8_t want_midi;                    /* hello: send this client SLOOP's MIDI out */
    uint8_t no_screen;                    /* hello: "screen": false (an editor: no screen, LEDs, status) */
    uint8_t sx[SLOOP_SYSEX_MAX + 2u];     /* sysex: the whole F0..F7 message */
    uint16_t sx_len;
    char err[64];                         /* why a message was rejected */
} sloop_proto_msg_t;

/* 0 = ok, < 0 = rejected (m->err says why) */
int sloop_proto_parse(const char *json, size_t len, sloop_proto_msg_t *m);
/* posts a parsed input message to the core's mailboxes; 0 = ok, -1 = mailbox full / not input */
int sloop_proto_apply(const sloop_proto_msg_t *m);

/* encoders: return the length written (without a terminating 0, which is added when room), or 0
 * when cap is too small */
size_t sloop_proto_hello(char *buf, size_t cap, const char *target);
size_t sloop_proto_leds(char *buf, size_t cap, const uint8_t led[SLOOP_LED_COUNT]);
size_t sloop_proto_status(char *buf, size_t cap, const sloop_status_t *st);
size_t sloop_proto_pong(char *buf, size_t cap, uint32_t n);
size_t sloop_proto_error(char *buf, size_t cap, const char *msg);
size_t sloop_proto_sysex(char *buf, size_t cap, const uint8_t *msg, size_t n);
size_t sloop_proto_midi(char *buf, size_t cap, uint32_t usb_midi_packet);
void sloop_proto_rect_header(uint8_t hdr[SLOOP_PROTO_RECT_HDR], const sloop_rect_t *r);

/* ---- the hub */
typedef struct {
    /* 0 sent (or queued), 1 busy (not sent: try later), < 0 the client is gone */
    int (*send_text)(void *ctx, const char *s, size_t n);
    int (*send_bin)(void *ctx, const uint8_t *p, size_t n);
} sloop_proto_ops_t;

typedef struct {
    void *ctx;                            /* the transport's handle of the client, NULL = free */
    uint8_t need_hello, need_full;
    uint32_t led_ver, status_ver;
    uint32_t held_btn, held_key;          /* what this client holds down: released if it goes away */
    uint8_t sysex, midi_out;              /* gets SLOOP's SysEx replies (an editor) / MIDI out */
    uint8_t no_screen;                    /* wants no screen, LEDs, status (an editor) */
    uint32_t rx, tx_rects, tx_full, dropped;
} sloop_proto_client_t;

typedef struct {
    sloop_proto_ops_t ops;
    const char *target;                   /* "host", "esp32s3", "esp32s3-qemu" */
    uint8_t *frame;                       /* scratch, SLOOP_PROTO_FRAME_BYTES */
    sloop_proto_client_t c[SLOOP_PROTO_MAX_CLIENTS];
    uint32_t pumps, rects;
} sloop_proto_hub_t;

void sloop_proto_hub_init(sloop_proto_hub_t *h, const sloop_proto_ops_t *ops, const char *target, uint8_t *frame);
int sloop_proto_client_add(sloop_proto_hub_t *h, void *ctx);      /* index, or -1 when full */
/* the client is gone: whatever it held down is released (a dropped connection never leaves a
 * button or key stuck down in SLOOP) */
void sloop_proto_client_remove(sloop_proto_hub_t *h, void *ctx);
int sloop_proto_clients(const sloop_proto_hub_t *h);
/* a text message from a client: parsed, applied, answered (pong / error / hello on request) */
void sloop_proto_on_text(sloop_proto_hub_t *h, void *ctx, const char *s, size_t n);
/* screen changes, LEDs and status to every client */
void sloop_proto_pump(sloop_proto_hub_t *h);

#ifdef __cplusplus
}
#endif
