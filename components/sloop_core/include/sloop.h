/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the SLOOP core as seen by a target (host app, ESP32 firmware, tests).
 *
 * The core is upstream SLOOP compiled against a virtual FM-1 (components/sloop_core/port).
 * A target runs two contexts (see sloop_platform.h):
 *
 *   UI context:     sloop_boot() once, then sloop_ui_step() about every millisecond
 *                   (the FM-1 main loop: input, LEDs, screen, autosave)
 *   audio context:  sloop_audio_render() whenever the output needs the next half buffer
 *                   (the FM-1 audio ISR: SLOOP_HALF_FRAMES stereo frames at SLOOP_FS)
 *
 * Transport tasks talk to the core only through the mailboxes below (thread-safe).
 * Controls are named by their printed label on the FM-1 and are wired like the real
 * panel (upstream panel.c PANEL_DEFAULT), so SLOOP's own input logic (holds, layers,
 * long presses, combinations, calibration) runs unchanged.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLOOP_FS 44100u
#define SLOOP_HALF_FRAMES 256u            /* frames per render (5.8 ms) */
#define SLOOP_LCD_W 240u
#define SLOOP_LCD_H 240u
#define SLOOP_LCD_BYTES (SLOOP_LCD_W * SLOOP_LCD_H * 2u)   /* RGB565, big-endian (as the ST7789 takes it) */
#define SLOOP_NKEYS 27u                   /* F3 .. G5: 16 white, 11 black */

typedef enum {                            /* the 14 buttons, by label (upstream panel.c B_*) */
    SLOOP_BTN_FX, SLOOP_BTN_SCL, SLOOP_BTN_ENV, SLOOP_BTN_LFO, SLOOP_BTN_EDIT, SLOOP_BTN_GLO,
    SLOOP_BTN_HOME, SLOOP_BTN_SAVE, SLOOP_BTN_ARP, SLOOP_BTN_SEQ, SLOOP_BTN_PLAY, SLOOP_BTN_REC,
    SLOOP_BTN_OCTDN, SLOOP_BTN_OCTUP, SLOOP_BTN_COUNT
} sloop_btn_t;

typedef enum {                            /* the 7 encoders, by label (upstream panel.c EN_*) */
    SLOOP_ENC_SELECT, SLOOP_ENC_ALGO, SLOOP_ENC_PRESETS, SLOOP_ENC_K1, SLOOP_ENC_K2, SLOOP_ENC_K3,
    SLOOP_ENC_K4, SLOOP_ENC_COUNT
} sloop_enc_t;

typedef enum { SLOOP_POT_MASTER, SLOOP_POT_COUNT } sloop_pot_t;   /* analog pots (0..1023) */

const char *sloop_btn_name(unsigned b);   /* "FX" .. "OCT+", NULL past the end */
const char *sloop_enc_name(unsigned e);   /* "SELECT" .. "K4" */
int sloop_btn_from_name(const char *s, size_t n);   /* -1 if unknown */
int sloop_enc_from_name(const char *s, size_t n);

/* ---- lifecycle (UI context) */
void sloop_boot(void);
uint32_t sloop_ui_step(void);             /* returns the ms until it wants to run again */

/* ---- audio (audio context): the next SLOOP_HALF_FRAMES frames, interleaved L/R int32,
 * 24-bit signed in the low bits (Q15 << 7: a -6 dBFS ceiling, as on the FM-1) */
const int32_t *sloop_audio_render(void);

/* ---- input mailbox (any task). Applied by the next sloop_ui_step, in order; a press and
 * release of the same control never land in the same UI pass. Return 0, or -1 when full. */
int sloop_post_button(unsigned btn, int down);
int sloop_post_key(unsigned key, int down);        /* 0 = F3 .. 26 = G5 */
int sloop_post_encoder(unsigned enc, int steps);   /* detent steps, + = clockwise */
int sloop_post_pot(unsigned pot, unsigned value);  /* 0..1023 */
int sloop_post_midi(uint32_t usb_midi_packet);     /* USB-MIDI event packet (cable 0) */

/* ---- SysEx / MIDI out. SLOOP's web editor protocol (upstream web/EDITOR_PROTOCOL.md) is SysEx
 * F0 7D 46 4C ... F7: requests go in with sloop_post_sysex (any task), replies and pushes come out
 * of sloop_take_sysex. Notes SLOOP sends (MIDI out) come out of sloop_take_midi. Nothing is
 * produced while no transport is attached (on the FM-1: unless a USB host configured the device).
 * Each transport attaches on its own and reads its own copy of everything SLOOP sends from the
 * moment it attached; it calls attach / take from one task (or under one lock). SLOOP waits for
 * the slowest attached transport (as for a slow USB host on the FM-1): a transport keeps reading,
 * dropping what its link cannot take, or detaches. */
#define SLOOP_SYSEX_MAX 640u                       /* bytes between F0 and F7 */
enum { SLOOP_MIDI_WEB, SLOOP_MIDI_USB, SLOOP_MIDI_PORTS };   /* the transports: WebSocket hub, USB-MIDI */
void sloop_midi_attach(unsigned port, int attached);
int sloop_post_sysex(const uint8_t *msg, size_t n); /* a whole F0..F7 message; 0, or -1 (full / bad) */
size_t sloop_take_sysex(unsigned port, uint8_t *buf, size_t cap);  /* a whole F0..F7 message, or 0 */
int sloop_take_midi(unsigned port, uint32_t *usb_midi_packet);      /* 1 and a packet, or 0 */

/* ---- display mailbox (any task) */
typedef struct { uint16_t x, y, w, h; } sloop_rect_t;
/* Take what changed since the last take (one bounding rectangle). Copies its pixels (row by
 * row, RGB565 big-endian) into dst when cap is large enough. Returns 1 if there was a change,
 * 0 if not, -1 if cap was too small (the change is kept). */
int sloop_display_take(sloop_rect_t *r, uint8_t *dst, size_t cap);
void sloop_display_copy_full(uint8_t *dst);        /* the whole published screen */
uint32_t sloop_display_frames(void);               /* screens published so far */

/* ---- LEDs: one byte per control, 0 off, 1 dim, 2 lit. Buttons first (sloop_btn_t order),
 * then the 27 keys. */
#define SLOOP_LED_COUNT (SLOOP_BTN_COUNT + SLOOP_NKEYS)
uint32_t sloop_leds_get(uint8_t out[SLOOP_LED_COUNT]);   /* returns a version, + 1 per change */

/* ---- status (any task): a few values for transports and tests */
typedef struct {
    uint32_t version;                     /* + 1 per change of the fields below */
    uint8_t playing, recording, sel_track, page;
    uint8_t menu;                         /* the HOME menu (or a dialog of it) is open */
    int16_t bpm;
    uint16_t cpu_q8;                      /* audio render load, 1/256 */
    uint32_t audio_halves, ui_frames, uptime_ms;
} sloop_status_t;
void sloop_status_get(sloop_status_t *st);

/* ---- a parameter by the label SLOOP shows for it ("LVL", "PAN", "BPM", ...; the first match):
 * track 0..3, or SLOOP_GLOBAL for the song's parameters. Read-only (any task); 1 if found. */
#define SLOOP_GLOBAL 0xFFu
int sloop_param_get(unsigned track, const char *label, int *value);

/* ---- the store as a file (backups). SLOOP keeps everything it saves in its FM-1 flash window
 * 0x90000..0xFFFFF (SLOOP_STORE_BYTES): settings, 4 projects + the autosave, 2 banks of user
 * presets (A/B copies with CRCs, upstream storage.c) and 3 user sample slots. sloop_store_check
 * reads an image of that window with SLOOP's own rules: 1 if it holds at least one valid object
 * (a SLOOP store), 0 if not (wrong size, blank, other data). Any task; touches nothing. */
#define SLOOP_STORE_BYTES 0x70000u
typedef struct {
    uint8_t settings;                     /* 1: valid */
    uint8_t projects;                     /* 0..4 saved projects */
    uint8_t autosave;                     /* 1: the working project */
    uint8_t preset_banks;                 /* 0..2 user preset banks */
    uint8_t samples;                      /* 0..3 user sample slots */
} sloop_store_info_t;
int sloop_store_check(const uint8_t *img, size_t n, sloop_store_info_t *info);

/* ---- cross-target check: a fixed 4-track song rendered without clock or UI (port/sloop_selftest.c).
 * Call on a fresh power-on, before sloop_boot. Returns an FNV-1a hash of the Q15 mix; equal on
 * every target that compiles SLOOP correctly. */
uint64_t sloop_selftest_render(uint32_t ms, int32_t *peak);

/* ---- identity */
const char *sloop_version(void);          /* SLOOP's version string */
const char *sloop_upstream_commit(void);  /* the upstream commit compiled in */

#ifdef __cplusplus
}
#endif
