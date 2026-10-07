/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: internal to the web_server component. */
#pragma once
#include "esp_http_server.h"

void sloop_settings_register(httpd_handle_t server);   /* settings.c: /api/settings */
void sloop_ota_register(httpd_handle_t server);        /* ota.c: /api/ota */
void sloop_store_register(httpd_handle_t server);      /* backup.c: /api/store */

/* (settings.c) a JSON reply, no caching */
esp_err_t sloop_web_json(httpd_req_t *req, const char *status, const char *json);
/* (settings.c) header X-Sloopy-Password (URI-encoded) is the current Wi-Fi password: for requests
 * whose body is not JSON (an image) */
int sloop_web_password_ok(httpd_req_t *req);
/* (settings.c) restart in a second (the reply goes out first) */
void sloop_web_restart_soon(void);
