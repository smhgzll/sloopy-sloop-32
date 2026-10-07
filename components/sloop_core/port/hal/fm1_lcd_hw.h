/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_lcd_hw.h).
 *
 * The FM-1 sends its screen to an ST7789 240x240 panel over SPI; upstream lcd.c speaks the
 * panel's command set. Here the bytes go to a virtual ST7789 that understands what lcd.c
 * sends (CASET 0x2A, RASET 0x2B, RAMWR 0x2C, the rest is accepted and ignored) and writes the
 * pixels, RGB565 big-endian as on the wire, into a 240x240 framebuffer. It also keeps the
 * bounding box of everything written since the port runtime last took it, so only what
 * changed is published to the browser. The framebuffer is a big zero-initialised buffer like
 * SLOOP's own canvases (section .pool: external RAM on the ESP32).
 */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

#define FM1_VLCD_W 240u
#define FM1_VLCD_H 240u

static uint8_t fm1_vlcd_fb[FM1_VLCD_W * FM1_VLCD_H * 2u] __attribute__((section(".pool"), aligned(4)));
static struct {
    uint8_t cmd;                  /* the command whose data follows */
    uint8_t argn;                 /* CASET / RASET bytes so far */
    uint8_t arg[4];
    uint8_t half;                 /* RAMWR: a pixel's first byte arrived, its second not yet */
    uint8_t on;                   /* DISPON seen */
    uint16_t xs, xe, ys, ye;      /* the window */
    uint16_t x, y;                /* RAMWR cursor */
    uint16_t dx0, dy0, dx1, dy1;  /* dirty box, inclusive; dx0 > dx1: clean */
    uint32_t pixels;              /* written so far (statistics) */
} fm1_vlcd = {0, 0, {0}, 0, 0, 0, FM1_VLCD_W - 1u, 0, FM1_VLCD_H - 1u, 0, 0, 0xFFFF, 0xFFFF, 0, 0, 0};
static uint32_t fm1_lcd_timeouts __attribute__((unused));

FM1_INLINE void fm1_lcd_hw_init(void) {}
FM1_INLINE void fm1_lcd_baud(uint32_t b) { (void)b; }
static inline void fm1_lcd_wait(void) {}
FM1_INLINE void fm1_lcd_deselect(void) {}

static void fm1_vlcd_dirty(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (fm1_vlcd.dx0 > fm1_vlcd.dx1) {
        fm1_vlcd.dx0 = (uint16_t)x0;
        fm1_vlcd.dy0 = (uint16_t)y0;
        fm1_vlcd.dx1 = (uint16_t)x1;
        fm1_vlcd.dy1 = (uint16_t)y1;
        return;
    }
    if (x0 < fm1_vlcd.dx0) fm1_vlcd.dx0 = (uint16_t)x0;
    if (y0 < fm1_vlcd.dy0) fm1_vlcd.dy0 = (uint16_t)y0;
    if (x1 > fm1_vlcd.dx1) fm1_vlcd.dx1 = (uint16_t)x1;
    if (y1 > fm1_vlcd.dy1) fm1_vlcd.dy1 = (uint16_t)y1;
}

static void fm1_lcd_send_cmd(uint8_t c)
{
    fm1_vlcd.cmd = c;
    fm1_vlcd.argn = 0;
    if (c == 0x2Cu) {                                  /* RAMWR: from the window's top left */
        fm1_vlcd.x = fm1_vlcd.xs;
        fm1_vlcd.y = fm1_vlcd.ys;
        fm1_vlcd.half = 0;
        if (fm1_vlcd.xs <= fm1_vlcd.xe && fm1_vlcd.ys <= fm1_vlcd.ye)
            fm1_vlcd_dirty(fm1_vlcd.xs, fm1_vlcd.ys, fm1_vlcd.xe, fm1_vlcd.ye);
    } else if (c == 0x29u) {
        fm1_vlcd.on = 1;
    } else if (c == 0x01u) {                           /* SWRESET */
        fm1_vlcd.on = 0;
        fm1_vlcd.xs = 0; fm1_vlcd.xe = FM1_VLCD_W - 1u;
        fm1_vlcd.ys = 0; fm1_vlcd.ye = FM1_VLCD_H - 1u;
    }
}

static uint16_t fm1_vlcd_clamp(uint32_t v, uint32_t max) { return (uint16_t)(v > max ? max : v); }

static void fm1_vlcd_pixels(const uint8_t *b, uint32_t n)
{
    uint32_t xs = fm1_vlcd.xs, xe = fm1_vlcd.xe, ys = fm1_vlcd.ys, ye = fm1_vlcd.ye;
    uint32_t x = fm1_vlcd.x, y = fm1_vlcd.y;
    if (xs > xe || ys > ye)
        return;
    if (fm1_vlcd.half && n) {                          /* the second byte of a split pixel */
        fm1_vlcd_fb[(y * FM1_VLCD_W + x) * 2u + 1u] = *b++;
        n--;
        fm1_vlcd.half = 0;
        fm1_vlcd.pixels++;
        if (++x > xe) { x = xs; if (++y > ye) y = ys; }
    }
    while (n >= 2u) {                                  /* whole row runs at a time */
        uint32_t run = xe - x + 1u, bytes, i;
        uint8_t *d = &fm1_vlcd_fb[(y * FM1_VLCD_W + x) * 2u];
        if (run > n / 2u)
            run = n / 2u;
        bytes = run * 2u;
        for (i = 0; i < bytes; i++)
            d[i] = b[i];
        b += bytes;
        n -= bytes;
        fm1_vlcd.pixels += run;
        x += run;
        if (x > xe) { x = xs; if (++y > ye) y = ys; }
    }
    if (n) {                                           /* a pixel split across transfers */
        fm1_vlcd_fb[(y * FM1_VLCD_W + x) * 2u] = *b;
        fm1_vlcd.half = 1;
    }
    fm1_vlcd.x = (uint16_t)x;
    fm1_vlcd.y = (uint16_t)y;
}

static void fm1_lcd_send_data(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i;
    switch (fm1_vlcd.cmd) {
    case 0x2Au:                                        /* CASET: xs, xe (big-endian) */
    case 0x2Bu:                                        /* RASET: ys, ye */
        for (i = 0; i < n && fm1_vlcd.argn < 4u; i++)
            fm1_vlcd.arg[fm1_vlcd.argn++] = b[i];
        if (fm1_vlcd.argn == 4u) {
            uint32_t a0 = (uint32_t)fm1_vlcd.arg[0] << 8 | fm1_vlcd.arg[1];
            uint32_t a1 = (uint32_t)fm1_vlcd.arg[2] << 8 | fm1_vlcd.arg[3];
            if (fm1_vlcd.cmd == 0x2Au) {
                fm1_vlcd.xs = fm1_vlcd_clamp(a0, FM1_VLCD_W - 1u);
                fm1_vlcd.xe = fm1_vlcd_clamp(a1, FM1_VLCD_W - 1u);
            } else {
                fm1_vlcd.ys = fm1_vlcd_clamp(a0, FM1_VLCD_H - 1u);
                fm1_vlcd.ye = fm1_vlcd_clamp(a1, FM1_VLCD_H - 1u);
            }
        }
        break;
    case 0x2Cu:                                        /* RAMWR */
        fm1_vlcd_pixels(b, n);
        break;
    default:                                           /* COLMOD, MADCTL, ...: as lcd.c sets them */
        break;
    }
}
