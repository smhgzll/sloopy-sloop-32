/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: a small HTTP + WebSocket server for the host app (one thread, poll()).
 * Serves static files from a web root and upgrades GET /ws to a WebSocket (RFC 6455). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct ws_server ws_server_t;
typedef struct ws_conn ws_conn_t;

typedef struct {
    void (*on_open)(void *user, ws_conn_t *c);
    void (*on_text)(void *user, ws_conn_t *c, const char *s, size_t n);
    void (*on_close)(void *user, ws_conn_t *c);
} ws_handlers_t;

/* bind: "127.0.0.1" (default) or "0.0.0.0"; returns NULL on failure (message on stderr) */
ws_server_t *ws_server_start(const char *bind, int port, const char *webroot, const ws_handlers_t *h, void *user);
/* GET /api/...: fn(path, out, cap) returns the JSON length, 0 = not found */
typedef size_t (*ws_api_fn)(const char *path, char *out, size_t cap);
void ws_server_set_api(ws_server_t *s, ws_api_fn fn);
/* another directory searched after the web root (generated files) */
void ws_server_add_root(ws_server_t *s, const char *dir);
void ws_server_poll(ws_server_t *s, int timeout_ms);
void ws_server_stop(ws_server_t *s);
int ws_server_port(const ws_server_t *s);

/* 0 queued, 1 busy (the client is behind: nothing queued), -1 the connection is gone */
int ws_send_text(ws_conn_t *c, const char *s, size_t n);
int ws_send_bin(ws_conn_t *c, const uint8_t *p, size_t n);

/* exposed for tests */
void ws_sha1(const uint8_t *data, size_t n, uint8_t out[20]);
size_t ws_base64(const uint8_t *in, size_t n, char *out, size_t cap);
void ws_accept_key(const char *client_key, char out[29]);
