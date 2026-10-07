/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: SLOOP's store as a file, from the device page.
 *
 * GET /api/store    the store window (SLOOP_STORE_BYTES: settings, projects + autosave, user
 *                   presets, user sample slots) as sloop-store-<hostname>.bin. Read while SLOOP
 *                   runs: storage.c's A/B copies keep every object whole (a sector caught in the
 *                   middle of a save has the previous copy next to it).
 * POST /api/store   a backup to restore, header X-Sloopy-Password = the current Wi-Fi password. It
 *                   must check as a SLOOP store (sloop_store_check); it is staged and the device
 *                   restarts: the boot copies it in before SLOOP starts (sloop_port_store_stage).
 */
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "sloop.h"
#include "sloop_platform.h"
#include "sloop_port.h"
#include "web_priv.h"
#include "web_server.h"

static const char *TAG = "backup";
#define CHUNK 4096u

static esp_err_t h_get(httpd_req_t *req)
{
    sloop_net_settings_t s;
    char disp[96];
    uint8_t *buf = malloc(CHUNK);
    uint32_t off;
    if (!buf)
        return sloop_web_json(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    sloop_settings_load(&s);
    snprintf(disp, sizeof disp, "attachment; filename=\"sloop-store-%s.bin\"", s.hostname);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (off = 0; off < SLOOP_STORE_BYTES; off += CHUNK) {
        if (sloop_plat_store_read(SLOOP_STORE_LO + off, buf, CHUNK) ||
            httpd_resp_send_chunk(req, (const char *)buf, CHUNK) != ESP_OK) {
            free(buf);
            ESP_LOGW(TAG, "backup interrupted at %u", (unsigned)off);
            return ESP_FAIL;                         /* (the connection is closed: an incomplete file) */
        }
    }
    free(buf);
    ESP_LOGI(TAG, "backup sent (%u KiB)", (unsigned)(SLOOP_STORE_BYTES / 1024u));
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_post(httpd_req_t *req)
{
    sloop_store_info_t info;
    uint8_t *img;
    size_t got = 0;
    int timeouts = 0;
    char js[160];
    esp_err_t err;
    if (!sloop_web_password_ok(req))
        return sloop_web_json(req, "403 Forbidden", "{\"error\":\"X-Sloopy-Password: the current Wi-Fi password does not match\"}");
    if (req->content_len != SLOOP_STORE_BYTES)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"a backup is exactly 458752 bytes (sloop-store-*.bin)\"}");
    img = heap_caps_malloc(SLOOP_STORE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img)
        return sloop_web_json(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    while (got < SLOOP_STORE_BYTES) {
        int r = httpd_req_recv(req, (char *)img + got, SLOOP_STORE_BYTES - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 6)
            continue;
        if (r <= 0) {
            free(img);
            return sloop_web_json(req, "400 Bad Request", "{\"error\":\"the upload was interrupted\"}");
        }
        got += (size_t)r;
    }
    if (!sloop_store_check(img, got, &info)) {
        free(img);
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"not a SLOOP store: no valid project, preset or setting in it\"}");
    }
    err = sloop_port_store_stage(img, got);
    free(img);
    if (err != ESP_OK) {
        snprintf(js, sizeof js, "{\"error\":\"staging the backup failed (%s)\"}", esp_err_to_name(err));
        return sloop_web_json(req, "500 Internal Server Error", js);
    }
    snprintf(js, sizeof js,
             "{\"ok\":true,\"restart\":true,\"settings\":%u,\"projects\":%u,\"autosave\":%u,\"preset_banks\":%u,"
             "\"samples\":%u}",
             info.settings, info.projects, info.autosave, info.preset_banks, info.samples);
    ESP_LOGI(TAG, "backup accepted: restarting to restore it");
    sloop_web_restart_soon();
    return sloop_web_json(req, "200 OK", js);
}

void sloop_store_register(httpd_handle_t server)
{
    httpd_uri_t get = {.uri = "/api/store", .method = HTTP_GET, .handler = h_get};
    httpd_uri_t post = {.uri = "/api/store", .method = HTTP_POST, .handler = h_post};
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
}
