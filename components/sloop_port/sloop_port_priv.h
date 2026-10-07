/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: internal to the sloop_port component. */
#pragma once
#include "sloop_port.h"

#define SLOOP_PART_SUBTYPE 0x40   /* data partition subtype of "sloop" (config/partitions.csv) */
#define SLOOP_STAGE_SUBTYPE 0x41  /* "stage": a backup waiting to be restored */

void sloop_plat_locks_init(void);
extern sloop_port_stats_t sloop_port_stats;
