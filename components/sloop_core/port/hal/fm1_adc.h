/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 virtual FM-1 HAL (replaces upstream firmware/hal/fm1_adc.h).
 * The FM-1 reads the MASTER pot (ch 4) and the battery divider (ch 3). The virtual panel
 * sets the MASTER pot; there is no battery, so ch 3 reads as "no conversion" (-1). */
#pragma once
#include <stdint.h>

enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
#define FM1_ADC_NCH 8u
/* 724: the master gain upstream starts at (song.master_q12 = 2048 = 724^2 >> 8) */
static volatile int32_t fm1_adc_virt[FM1_ADC_NCH] = {-1, -1, -1, -1, 724, -1, -1, -1};

static inline void fm1_adc_init(void) {}
static inline int32_t fm1_adc_read(uint32_t ch) { return ch < FM1_ADC_NCH ? fm1_adc_virt[ch] : -1; }
