/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_audio.h).
 *
 * The FM-1 plays a double buffer through the ALNK0 DMA: each time a half has gone out, the
 * ALNK0 interrupt asks for the next one and audio.c's ISR renders it. Here the platform's
 * audio driver plays the role of the DMA: fm1_alnk_next() flips to the other half, raises
 * the "half free" pending and runs the ISR SLOOP registered, exactly once per half, then
 * hands the rendered half (int32 L/R, 24-bit) to the driver (I2S, a file, the null sink).
 */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

#define FM1_AUDIO_HALF 0x80u      /* pending: a half buffer is free */

static struct {
    int32_t *buf;
    uint32_t half_words;
    void (*isr)(void);
    volatile uint8_t pend;
    volatile uint8_t half;        /* the half being filled */
    volatile uint8_t run;         /* the "DMA" plays (fm1_audio_stop clears it) */
} fm1_alnk;

FM1_INLINE void fm1_audio_init(int32_t *buf, uint32_t half_words, void (*isr)(void), uint32_t prio)
{
    (void)prio;
    fm1_alnk.buf = buf;
    fm1_alnk.half_words = half_words;
    fm1_alnk.isr = isr;
    fm1_alnk.half = 1;
    fm1_alnk.pend = 0;
    fm1_alnk.run = 1;
}

FM1_INLINE uint8_t fm1_audio_pending(void) { return fm1_alnk.pend; }
FM1_INLINE void fm1_audio_ack_aux(uint8_t p) { (void)p; }
FM1_INLINE uint32_t fm1_audio_free_half(void) { return fm1_alnk.half; }
FM1_INLINE void fm1_audio_ack_half(void) { fm1_alnk.pend = 0; }
FM1_INLINE void fm1_audio_stop(void) { fm1_alnk.run = 0; }

/* the port's DMA: the next half, rendered by SLOOP's ISR; NULL before fm1_audio_init or after
 * fm1_audio_stop (the driver then plays silence) */
static const int32_t *fm1_alnk_next(void)
{
    if (!fm1_alnk.run || !fm1_alnk.isr)
        return 0;
    fm1_alnk.half ^= 1u;
    fm1_alnk.pend = FM1_AUDIO_HALF;
    fm1_alnk.isr();
    return fm1_alnk.buf + fm1_alnk.half * fm1_alnk.half_words;
}
