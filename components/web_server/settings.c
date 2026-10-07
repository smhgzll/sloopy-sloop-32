/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: network settings at runtime (NVS namespace "sloopy"), over the build-time values
 * from config/local.env (Kconfig). GET /api/settings shows them (never the password); POST changes
 * them, guarded by the current Wi-Fi password; POST /api/settings/reset returns to the build values.
 * Changes apply at the next boot.
 */
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "web_priv.h"
#include "web_server.h"

static const char *TAG = "settings";
#define NS "sloopy"

static void get_str(nvs_handle_t h, const char *key, const char *def, char *out, size_t cap)
{
    size_t n = cap;
    if (!h || nvs_get_str(h, key, out, &n) != ESP_OK) {
        strncpy(out, def, cap - 1);
        out[cap - 1] = 0;
    }
}

void sloop_settings_load(sloop_net_settings_t *s)
{
    nvs_handle_t h = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        h = 0;
    get_str(h, "wifi_mode", CONFIG_SLOOPY_WIFI_MODE, s->mode, sizeof s->mode);
    get_str(h, "wifi_ssid", CONFIG_SLOOPY_WIFI_SSID, s->ssid, sizeof s->ssid);
    get_str(h, "wifi_pass", CONFIG_SLOOPY_WIFI_PASSWORD, s->password, sizeof s->password);
    get_str(h, "hostname", CONFIG_SLOOPY_HOSTNAME, s->hostname, sizeof s->hostname);
    s->from_nvs = 0;
    if (h) {
        size_t n = 0;
        s->from_nvs = nvs_get_str(h, "wifi_ssid", NULL, &n) == ESP_OK;
        nvs_close(h);
    }
}

/* ---- a tiny reader for the flat JSON the settings page posts */
static int json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[40];
    const char *p;
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return -1;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c != '"' && c != '\\' && c != '/')
                return -1;
        }
        if ((unsigned char)c < 0x20 || n + 1 >= cap)
            return -1;
        out[n++] = c;
    }
    if (*p != '"')
        return -1;
    out[n] = 0;
    return 1;
}

static void json_escape(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 2 < cap; s++) {
        if (*s == '"' || *s == '\\')
            dst[n++] = '\\';
        dst[n++] = (unsigned char)*s < 0x20 ? '?' : *s;
    }
    dst[n] = 0;
}

esp_err_t sloop_web_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t h_get(httpd_req_t *req)
{
    sloop_net_settings_t s;
    char buf[400], ssid[80], host[80], mode[16];
    sloop_settings_load(&s);
    json_escape(ssid, sizeof ssid, s.ssid);
    json_escape(host, sizeof host, s.hostname);
    json_escape(mode, sizeof mode, s.mode);
    snprintf(buf, sizeof buf,
             "{\"mode\":\"%s\",\"ssid\":\"%s\",\"hostname\":\"%s\",\"password_set\":%s,\"source\":\"%s\","
             "\"network\":\"%s\"}",
             mode, ssid, host, s.password[0] ? "true" : "false", s.from_nvs ? "device" : "build",
#if CONFIG_ETH_USE_OPENETH
             "ethernet (QEMU)"
#else
             "wifi"
#endif
    );
    return sloop_web_json(req, "200 OK", buf);
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int got = 0;
    if (req->content_len >= cap)
        return -1;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - (size_t)got);
        if (r <= 0)
            return -1;
        got += r;
    }
    buf[got] = 0;
    return got;
}

static int authorised(const char *body, const sloop_net_settings_t *cur)
{
    char given[72];
    if (json_str(body, "current_password", given, sizeof given) != 1)
        given[0] = 0;
    return !strcmp(given, cur->password);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
void sloop_web_restart_soon(void) { xTaskCreate(restart_task, "restart", 2048, NULL, 1, NULL); }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

int sloop_web_password_ok(httpd_req_t *req)
{
    sloop_net_settings_t cur;
    char raw[200], pw[72];
    size_t n = 0;
    const char *s = raw;
    if (httpd_req_get_hdr_value_str(req, "X-Sloopy-Password", raw, sizeof raw) != ESP_OK)
        raw[0] = 0;
    while (*s && n + 1 < sizeof pw) {               /* URI-decode */
        if (s[0] == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
            pw[n++] = (char)(hexv(s[1]) << 4 | hexv(s[2]));
            s += 3;
        } else {
            pw[n++] = *s++;
        }
    }
    pw[n] = 0;
    sloop_settings_load(&cur);
    return !strcmp(pw, cur.password);
}

static esp_err_t h_post(httpd_req_t *req)
{
    char body[512];
    sloop_net_settings_t cur, s;
    nvs_handle_t h;
    int restart = 0;
    if (read_body(req, body, sizeof body) < 0)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"body too long or unreadable\"}");
    sloop_settings_load(&cur);
    if (!authorised(body, &cur))
        return sloop_web_json(req, "403 Forbidden", "{\"error\":\"current_password does not match\"}");
    s = cur;
    if (json_str(body, "mode", s.mode, sizeof s.mode) < 0 || json_str(body, "ssid", s.ssid, sizeof s.ssid) < 0 ||
        json_str(body, "password", s.password, sizeof s.password) < 0 ||
        json_str(body, "hostname", s.hostname, sizeof s.hostname) < 0)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"malformed field\"}");
    if (strcmp(s.mode, "AP") && strcmp(s.mode, "STA"))
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"mode must be AP or STA\"}");
    if (!s.ssid[0] || strlen(s.ssid) > 32)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"ssid: 1..32 characters\"}");
    if (s.password[0] && (strlen(s.password) < 8 || strlen(s.password) > 63))
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"password: 8..63 characters (or empty: open network)\"}");
    if (!s.hostname[0] || strlen(s.hostname) > 31 || strspn(s.hostname, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(s.hostname))
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"hostname: a-z, 0-9, - (1..31)\"}");
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return sloop_web_json(req, "500 Internal Server Error", "{\"error\":\"nvs\"}");
    nvs_set_str(h, "wifi_mode", s.mode);
    nvs_set_str(h, "wifi_ssid", s.ssid);
    nvs_set_str(h, "wifi_pass", s.password);
    nvs_set_str(h, "hostname", s.hostname);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "network settings changed: %s \"%s\", hostname %s", s.mode, s.ssid, s.hostname);
#if !CONFIG_ETH_USE_OPENETH
    restart = strstr(body, "\"restart\":true") != NULL;
#endif
    if (restart)
        sloop_web_restart_soon();
    return sloop_web_json(req, "200 OK", restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"applies\":\"next boot\"}");
}

static esp_err_t h_reset(httpd_req_t *req)
{
    char body[256];
    sloop_net_settings_t cur;
    nvs_handle_t h;
    if (read_body(req, body, sizeof body) < 0)
        return sloop_web_json(req, "400 Bad Request", "{\"error\":\"body\"}");
    sloop_settings_load(&cur);
    if (!authorised(body, &cur))
        return sloop_web_json(req, "403 Forbidden", "{\"error\":\"current_password does not match\"}");
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_mode");
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        nvs_erase_key(h, "hostname");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "network settings back to the build's");
    return sloop_web_json(req, "200 OK", "{\"ok\":true,\"applies\":\"next boot\"}");
}

void sloop_settings_register(httpd_handle_t server)
{
    httpd_uri_t get = {.uri = "/api/settings", .method = HTTP_GET, .handler = h_get};
    httpd_uri_t post = {.uri = "/api/settings", .method = HTTP_POST, .handler = h_post};
    httpd_uri_t reset = {.uri = "/api/settings/reset", .method = HTTP_POST, .handler = h_reset};
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
    httpd_register_uri_handler(server, &reset);
}
