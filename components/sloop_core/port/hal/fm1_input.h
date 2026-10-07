/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_input.h).
 *
 * The FM-1 scans an 11 x 6 key / button / encoder matrix and drives the LEDs on the same
 * columns. Here the matrix is virtual: the port runtime presses and releases matrix ids
 * and turns matrix encoders (fm1_virt_*), and SLOOP reads the same debounced state, edges
 * and detent steps it reads on the device. The id layout, FM1_KEYMAP (used by the LED
 * helpers and ui_input.c) and the LED picture (fm1_led / fm1_led_dim) are the FM-1's, so
 * the port can read back which control is lit.
 *
 * Ids: 0..13 buttons, 14..40 note keys (bit n of notes = key n, 0 = F3 .. 26 = G5).
 * Shared with the audio context: notes / buttons (read by seq.c) are single-word stores.
 */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_irq.h"

#define FM1_NCOL 11u
#define FM1_NKEY 41u
#define FM1_NENC 7u

/* key id at (physical column, packed row bit), -1 = none (upstream table) */
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PA0: encoders */
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},          /* PA5 */
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},          /* PA6 */
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},          /* PA7 */
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},          /* PA8 */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PB7: encoder 6 */
};
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };

static volatile struct {
    uint32_t notes;              /* bit n = note key n */
    uint32_t buttons;            /* bit i = button i (0..13) */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    int16_t enc_steps[FM1_NENC]; /* + = clockwise */
    uint32_t frames;
} fm1_in;
static uint8_t fm1_led[FM1_NCOL];
#ifndef FM1_LED_DIM_MASK
#define FM1_LED_DIM_MASK 3u
#endif
static uint8_t fm1_led_dim[FM1_NCOL];

static inline uint32_t fm1__lock(void)
{
    fm1_irq_off();
    return 0;
}
static inline void fm1__unlock(uint32_t v)
{
    (void)v;
    fm1_irq_on();
}

static void fm1_input_init(void)
{
    uint32_t i;
    fm1_in.notes = fm1_in.buttons = fm1_in.pressed = fm1_in.released = fm1_in.notes_pressed = 0;
    for (i = 0; i < FM1_NENC; i++)
        fm1_in.enc_steps[i] = 0;
}
static inline void fm1_input_scan(void) { fm1_in.frames++; }
static inline void fm1_input_tick(void) {}

/* ---- the virtual matrix (port runtime, UI context) */
static void fm1_virt_key(uint32_t id, int down)       /* a button (0..13) or note key (14..40) */
{
    uint32_t k = fm1__lock();
    if (id >= 14u && id < FM1_NKEY) {
        uint32_t m = 1u << (id - 14u);
        if (down) {
            if (!(fm1_in.notes & m))
                fm1_in.notes_pressed |= m;
            fm1_in.notes |= m;
        } else {
            fm1_in.notes &= ~m;
        }
    } else if (id < 14u && ((fm1_in.buttons >> id) & 1u) != (down ? 1u : 0u)) {
        fm1_in.buttons ^= 1u << id;
        if (down)
            fm1_in.pressed |= 1u << id;
        else
            fm1_in.released |= 1u << id;
    }
    fm1__unlock(k);
}
static void fm1_virt_enc(uint32_t e, int32_t steps)
{
    uint32_t k = fm1__lock();
    if (e < FM1_NENC) {
        int32_t s = fm1_in.enc_steps[e] + steps;
        fm1_in.enc_steps[e] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    fm1__unlock(k);
}

/* ---- SLOOP's side (as upstream) */
static int32_t fm1_enc_take(uint32_t e)
{
    uint32_t k = fm1__lock();
    int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    fm1__unlock(k);
    return s;
}

static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    fm1__unlock(k);
    return p;
}

static uint32_t fm1_input_note_edges(void)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    fm1__unlock(k);
    return p;
}

/* LED of key id (button 0..13 or note key 14..40) */
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led[p] |= (uint8_t)(1u << r);
                else
                    fm1_led[p] &= (uint8_t)~(1u << r);
            }
}

/* ---- read back (port runtime): 0 off, 1 dim, 2 lit */
static uint32_t fm1_virt_led(uint32_t id)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id)
                return ((fm1_led[p] >> r) & 1u) ? 2u : ((fm1_led_dim[p] >> r) & 1u) ? 1u : 0u;
    return 0;
}
