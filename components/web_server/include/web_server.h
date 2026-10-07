/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: the network and the web panel on the ESP32-S3.
 *
 *   sloop_net_start()   Wi-Fi (STA, falling back to its own AP; or AP) from Kconfig / local.env,
 *                       or under QEMU the emulated OpenCores Ethernet (CONFIG_ETH_USE_OPENETH)
 *   sloop_web_start()   HTTP: the panel (gzip, embedded in the firmware) and the WebSocket /ws
 *                       speaking docs/PROTOCOL.md through components/sloop_proto
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the network settings in effect: NVS (changed from the device page) over the build's (local.env) */
typedef struct {
    char mode[8];                 /* "AP" / "STA" */
    char ssid[33];
    char password[65];
    char hostname[32];
    int from_nvs;                 /* 1: changed on the device, 0: the build's values */
} sloop_net_settings_t;
void sloop_settings_load(sloop_net_settings_t *s);

esp_err_t sloop_net_start(void);
/* waits up to timeout_ms for an IP address; returns it as text ("" if none) */
const char *sloop_net_wait_ip(uint32_t timeout_ms);
esp_err_t sloop_web_start(const char *target, uint16_t port);

typedef struct {
    uint32_t clients, pumps, rects, http_requests, ws_messages;
} sloop_web_stats_t;
void sloop_web_get_stats(sloop_web_stats_t *st);

/* firmware update (ota.c): POST /api/ota writes a new image into the other OTA slot and restarts
 * into it. The new image boots pending verification: sloop_ota_confirm(1) keeps it, (0) or a reset
 * before that returns to the previous image (bootloader rollback). */
int sloop_ota_pending(void);              /* 1: the first boot of an update, not yet confirmed */
void sloop_ota_confirm(int ok);           /* (does nothing unless pending) */
const char *sloop_ota_state(void);        /* "valid", "pending", ..., "flashed" (written over USB) */

/* GET /api/status: the application fills a JSON object (diagnostics for hardware validation) */
typedef size_t (*sloop_web_status_fn)(char *buf, size_t cap);
void sloop_web_set_status_fn(sloop_web_status_fn fn);

#ifdef __cplusplus
}
#endif
