/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: firmware update from the browser (the FM-1 updates from SLOOP's browser
 * installer; here the device page uploads a build's sloopy_sloop_32.bin).
 *
 * POST /api/ota, body = the application image, header X-Sloopy-Password = the current Wi-Fi
 * password (URI-encoded), as /api/settings asks for it. The first 4 KB must be an ESP32-S3 image
 * of this project; it is written into the other OTA slot sector by sector, verified as a whole
 * (esp_ota_end), made the boot slot, and the device restarts into it.
 *
 * The new image boots "pending verification" (bootloader rollback): sloop_ota_confirm() keeps it
 * once SLOOP runs and the web server is up, so it can be updated again; if it resets before that,
 * the bootloader starts the previous image again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "web_priv.h"
#include "web_server.h"

static const char *TAG = "ota";
#define CHUNK 4096u
#define DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static volatile int s_busy;

/* the start of the upload: an ESP32-S3 application image of this project */
static const char *check_image(const uint8_t *p, size_t n)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)p;
    esp_app_desc_t d;
    if (n < DESC_OFF + sizeof d || ih->magic != ESP_IMAGE_HEADER_MAGIC)
        return "not an ESP-IDF application image (a build's build-hw/sloopy_sloop_32.bin)";
    if (ih->chip_id != ESP_CHIP_ID_ESP32S3)
        return "an image for another chip (not ESP32-S3)";
    memcpy(&d, p + DESC_OFF, sizeof d);
    if (d.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "the image has no application description";
    if (strncmp(d.project_name, esp_app_get_description()->project_name, sizeof d.project_name))
        return "an image of another project (not sloopy_sloop_32)";
    return NULL;
}

/* fills buf with exactly want bytes of the body (or fewer at its end): bytes, or -1 */
static int recv_full(httpd_req_t *req, uint8_t *buf, size_t want)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < want) {
        int r = httpd_req_recv(req, (char *)buf + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 6)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return (int)got;
}

static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    char js[160];
    const char *why = NULL;
    uint8_t *buf;
    size_t done = 0, total = req->content_len;
    esp_err_t err;

    if (!sloop_web_password_ok(req))
        return sloop_web_json(req, "403 Forbidden", "{\"error\":\"X-Sloopy-Password: the current Wi-Fi password does not match\"}");
    if (!next)
        return sloop_web_json(req, "500 Internal Server Error", "{\"error\":\"no OTA slot in the partition table\"}");
    if (total < DESC_OFF + sizeof(esp_app_desc_t) || total > next->size)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"the image size does not fit the OTA slot\"}");
    if (s_busy)
        return sloop_web_json(req, "409 Conflict", "{\"error\":\"an update is already running\"}");
    buf = malloc(CHUNK);
    if (!buf)
        return sloop_web_json(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    s_busy = 1;
    ESP_LOGI(TAG, "update: %u bytes into %s", (unsigned)total, next->label);
    while (done < total) {
        int n = recv_full(req, buf, total - done < CHUNK ? total - done : CHUNK);
        if (n <= 0) {
            why = "the upload was interrupted";
            break;
        }
        if (!done) {
            if ((why = check_image(buf, (size_t)n)) != NULL)
                break;
            err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err != ESP_OK) {
                why = "cannot open the OTA slot";
                break;
            }
        }
        if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) {
            why = "writing the OTA slot failed";
            break;
        }
        done += (size_t)n;
    }
    free(buf);
    if (why) {
        if (h)
            esp_ota_abort(h);
        s_busy = 0;
        ESP_LOGW(TAG, "update refused: %s", why);
        snprintf(js, sizeof js, "{\"error\":\"%s\"}", why);
        return sloop_web_json(req, "400 Bad Request", js);
    }
    err = esp_ota_end(h);
    if (err == ESP_OK)
        err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        s_busy = 0;
        ESP_LOGW(TAG, "update refused: %s", esp_err_to_name(err));
        snprintf(js, sizeof js, "{\"error\":\"the image does not verify (%s)\"}", esp_err_to_name(err));
        return sloop_web_json(req, "400 Bad Request", js);
    }
    ESP_LOGI(TAG, "update written to %s: restarting into it", next->label);
    snprintf(js, sizeof js, "{\"ok\":true,\"restart\":true,\"partition\":\"%s\"}", next->label);
    sloop_web_restart_soon();                   /* (s_busy stays: nothing more until then) */
    return sloop_web_json(req, "200 OK", js);
}

void sloop_ota_register(httpd_handle_t server)
{
    httpd_uri_t ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota};
    httpd_register_uri_handler(server, &ota);
}

int sloop_ota_pending(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void sloop_ota_confirm(int ok)
{
    if (!sloop_ota_pending())
        return;
    if (ok) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "SLOOPY_OTA_CONFIRMED %s: this image is kept", esp_ota_get_running_partition()->label);
    } else {
        ESP_LOGE(TAG, "the new image does not run SLOOP and the web server: back to the previous one");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

const char *sloop_ota_state(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK)
        return "flashed";                        /* (written over USB / serial, not by an update) */
    switch (st) {
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}
