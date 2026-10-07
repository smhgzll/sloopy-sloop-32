/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32: a small HTTP + WebSocket server for the host app (see ws_server.h).
 * Non-blocking sockets, one poll() loop, an output queue per connection with a cap (a client
 * that falls behind reports "busy" instead of growing memory). Only what the panel needs:
 * GET of static files, the WebSocket upgrade on /ws, text / binary / ping / close frames. */
#define _GNU_SOURCE
#include "ws_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONN 16
#define IN_CAP 70000u                 /* request headers / one client message */
#define OUT_BUSY (1024u * 1024u)      /* queued bytes above which sends report busy */

struct ws_conn {
    int fd;
    int ws;                           /* 1 after the upgrade */
    int closing;                      /* close once the output is flushed */
    uint8_t *in;
    size_t in_n;
    uint8_t *out;
    size_t out_n, out_cap, out_off;
    uint8_t *msg;                     /* a fragmented message being assembled */
    size_t msg_n;
    int msg_op;
};

struct ws_server {
    int lfd, port;
    char root[512], root2[512];
    ws_api_fn api;
    ws_handlers_t h;
    void *user;
    ws_conn_t c[MAX_CONN];
};

/* --------------------------------------------------------------- SHA-1 --- */
static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

void ws_sha1(const uint8_t *data, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bits = (uint64_t)n * 8u;
    size_t total = ((n + 8u) / 64u + 1u) * 64u, off, i;
    for (off = 0; off < total; off += 64u) {
        uint32_t w[80], a, b, c, d, e;
        for (i = 0; i < 64u; i++) {
            size_t k = off + i;
            uint8_t byte = k < n ? data[k] : k == n ? 0x80u : k >= total - 8u ? (uint8_t)(bits >> (8u * (total - 1u - k))) : 0u;
            if (i % 4u == 0u)
                w[i / 4u] = 0;
            w[i / 4u] |= (uint32_t)byte << (24u - 8u * (i % 4u));
        }
        for (i = 16; i < 80u; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (i = 0; i < 80u; i++) {
            uint32_t f, k, t;
            if (i < 20u) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40u) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60u) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (i = 0; i < 20u; i++)
        out[i] = (uint8_t)(h[i / 4u] >> (24u - 8u * (i % 4u)));
}

size_t ws_base64(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    if (cap < (n + 2u) / 3u * 4u + 1u)
        return 0;
    for (i = 0; i < n; i += 3u) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1u < n ? (uint32_t)in[i + 1] << 8 : 0u) | (i + 2u < n ? in[i + 2] : 0u);
        out[o++] = T[v >> 18 & 63u];
        out[o++] = T[v >> 12 & 63u];
        out[o++] = i + 1u < n ? T[v >> 6 & 63u] : '=';
        out[o++] = i + 2u < n ? T[v & 63u] : '=';
    }
    out[o] = 0;
    return o;
}

void ws_accept_key(const char *client_key, char out[29])
{
    char buf[128];
    uint8_t dig[20];
    snprintf(buf, sizeof buf, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key);
    ws_sha1((const uint8_t *)buf, strlen(buf), dig);
    ws_base64(dig, 20, out, 29);
}

/* -------------------------------------------------------------- output --- */
static int queue(ws_conn_t *c, const void *p, size_t n)
{
    if (c->out_n + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 65536u;
        uint8_t *nb;
        while (cap < c->out_n + n)
            cap *= 2u;
        nb = realloc(c->out, cap);
        if (!nb)
            return -1;
        c->out = nb;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_n, p, n);
    c->out_n += n;
    return 0;
}

static void flush(ws_conn_t *c)
{
    while (c->out_off < c->out_n) {
        ssize_t w = send(c->fd, c->out + c->out_off, c->out_n - c->out_off, MSG_NOSIGNAL);
        if (w <= 0)
            break;
        c->out_off += (size_t)w;
    }
    if (c->out_off == c->out_n) {
        c->out_off = c->out_n = 0;
    } else if (c->out_off > 262144u) {
        memmove(c->out, c->out + c->out_off, c->out_n - c->out_off);
        c->out_n -= c->out_off;
        c->out_off = 0;
    }
}

static int ws_frame(ws_conn_t *c, int op, const void *p, size_t n)
{
    uint8_t hdr[10];
    size_t h = 2;
    if (c->fd < 0 || !c->ws)
        return -1;
    if (c->out_n - c->out_off > OUT_BUSY)
        return 1;
    hdr[0] = (uint8_t)(0x80 | op);
    if (n < 126u) {
        hdr[1] = (uint8_t)n;
    } else if (n < 65536u) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)n;
        h = 4;
    } else {
        int i;
        hdr[1] = 127;
        for (i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
        h = 10;
    }
    if (queue(c, hdr, h) || queue(c, p, n))
        return -1;
    flush(c);
    return 0;
}

int ws_send_text(ws_conn_t *c, const char *s, size_t n) { return ws_frame(c, 0x1, s, n); }
int ws_send_bin(ws_conn_t *c, const uint8_t *p, size_t n) { return ws_frame(c, 0x2, p, n); }

/* ------------------------------------------------------------------ HTTP --- */
static void conn_close(ws_server_t *s, ws_conn_t *c)
{
    if (c->fd < 0)
        return;
    if (c->ws && s->h.on_close)
        s->h.on_close(s->user, c);
    close(c->fd);
    free(c->in);
    free(c->out);
    free(c->msg);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static void http_reply(ws_conn_t *c, int code, const char *status, const char *type, const void *body, size_t n)
{
    char h[512];
    int k = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n"
                     "Connection: close\r\n\r\n",
                     code, status, type, n);
    queue(c, h, (size_t)k);
    if (n)
        queue(c, body, n);
    c->closing = 1;
    flush(c);
}

static const char *mime(const char *path)
{
    static const char *const M[][2] = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".json", "application/json"},
        {".ttf", "font/ttf"}, {".woff2", "font/woff2"}, {".ico", "image/x-icon"}, {".txt", "text/plain; charset=utf-8"},
        {".md", "text/plain; charset=utf-8"}};
    const char *dot = strrchr(path, '.');
    size_t i;
    for (i = 0; dot && i < sizeof M / sizeof M[0]; i++)
        if (!strcasecmp(dot, M[i][0]))
            return M[i][1];
    return "application/octet-stream";
}

static const char *header(const char *req, const char *name, char *val, size_t cap)
{
    const char *p = req;
    size_t nl = strlen(name);
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1, *e;
            size_t n;
            while (*v == ' ')
                v++;
            e = strstr(v, "\r\n");
            n = e ? (size_t)(e - v) : strlen(v);
            if (n >= cap)
                n = cap - 1;
            memcpy(val, v, n);
            val[n] = 0;
            return val;
        }
    }
    return NULL;
}

static void serve_file(ws_server_t *s, ws_conn_t *c, const char *path)
{
    char full[1024];
    struct stat st;
    FILE *f;
    void *body;
    if (path[0] != '/' || strstr(path, "..") || strchr(path, '\\')) {
        http_reply(c, 400, "Bad Request", "text/plain", "bad path\n", 9);
        return;
    }
    if (snprintf(full, sizeof full, "%s%s%s", s->root, path, path[strlen(path) - 1] == '/' ? "index.html" : "") >=
        (int)sizeof full) {
        http_reply(c, 414, "URI Too Long", "text/plain", "", 0);
        return;
    }
    if ((stat(full, &st) || !S_ISREG(st.st_mode)) && s->root2[0] &&
        snprintf(full, sizeof full, "%s%s", s->root2, path) >= (int)sizeof full) {
        http_reply(c, 414, "URI Too Long", "text/plain", "", 0);
        return;
    }
    if (stat(full, &st) || !S_ISREG(st.st_mode) || !(f = fopen(full, "rb"))) {
        http_reply(c, 404, "Not Found", "text/plain", "not found\n", 10);
        return;
    }
    body = malloc((size_t)st.st_size + 1u);
    if (body && fread(body, 1, (size_t)st.st_size, f) == (size_t)st.st_size)
        http_reply(c, 200, "OK", mime(full), body, (size_t)st.st_size);
    else
        http_reply(c, 500, "Error", "text/plain", "read error\n", 11);
    free(body);
    fclose(f);
}

static void http_request(ws_server_t *s, ws_conn_t *c)
{
    char method[8], path[512], key[64], up[32];
    char *end = memmem(c->in, c->in_n, "\r\n\r\n", 4), *q;
    size_t used;
    if (!end) {
        if (c->in_n > 16384u)
            http_reply(c, 431, "Too Large", "text/plain", "", 0);
        return;
    }
    *end = 0;
    used = (size_t)(end - (char *)c->in) + 4u;
    if (sscanf((char *)c->in, "%7s %511s", method, path) != 2 || strcmp(method, "GET")) {
        http_reply(c, 405, "Method Not Allowed", "text/plain", "GET only\n", 9);
        return;
    }
    if ((q = strchr(path, '?')) != NULL)
        *q = 0;
    if (!strcmp(path, "/ws")) {
        char acc[29], resp[256];
        int k;
        if (!header((char *)c->in, "Sec-WebSocket-Key", key, sizeof key) ||
            !header((char *)c->in, "Upgrade", up, sizeof up) || strcasecmp(up, "websocket")) {
            http_reply(c, 400, "Bad Request", "text/plain", "websocket only\n", 15);
            return;
        }
        ws_accept_key(key, acc);
        k = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
        queue(c, resp, (size_t)k);
        flush(c);
        c->ws = 1;
        memmove(c->in, c->in + used, c->in_n - used);
        c->in_n -= used;
        {
            int one = 1;
            setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        if (s->h.on_open)
            s->h.on_open(s->user, c);
        return;
    }
    if (!strncmp(path, "/api/", 5)) {
        static char out[2048];
        size_t n = s->api ? s->api(path, out, sizeof out) : 0;
        if (n)
            http_reply(c, 200, "OK", "application/json", out, n);
        else
            http_reply(c, 404, "Not Found", "application/json", "{}", 2);
        return;
    }
    serve_file(s, c, path);
}

/* ------------------------------------------------------------ WebSocket --- */
static void ws_input(ws_server_t *s, ws_conn_t *c)
{
    for (;;) {
        uint8_t *b = c->in;
        size_t n = c->in_n, h = 2, len, i;
        int fin, op, masked;
        uint8_t mask[4];
        if (n < 2)
            return;
        fin = b[0] >> 7;
        op = b[0] & 0x0F;
        masked = b[1] >> 7;
        len = b[1] & 0x7Fu;
        if (len == 126u) {
            if (n < 4)
                return;
            len = (size_t)b[2] << 8 | b[3];
            h = 4;
        } else if (len == 127u) {
            if (n < 10)
                return;
            for (len = 0, i = 0; i < 8; i++)
                len = len << 8 | b[2 + i];
            h = 10;
        }
        if (!masked || len > IN_CAP - 16u) {               /* clients must mask; messages are small */
            c->closing = 1;
            return;
        }
        if (n < h + 4u + len)
            return;
        memcpy(mask, b + h, 4);
        for (i = 0; i < len; i++)
            b[h + 4u + i] ^= mask[i & 3u];
        {
            uint8_t *p = b + h + 4u;
            if (op == 0x8) {                                /* close: echo, then go */
                ws_frame(c, 0x8, p, len < 2 ? len : 2);
                c->closing = 1;
            } else if (op == 0x9) {
                ws_frame(c, 0xA, p, len);
            } else if (op == 0x1 || op == 0x2 || op == 0x0) {
                if (op != 0x0) {
                    free(c->msg);
                    c->msg = NULL;
                    c->msg_n = 0;
                    c->msg_op = op;
                }
                if (fin && !c->msg) {
                    if (c->msg_op == 0x1 && s->h.on_text)
                        s->h.on_text(s->user, c, (const char *)p, len);
                } else {
                    uint8_t *nm = realloc(c->msg, c->msg_n + len + 1u);
                    if (!nm || c->msg_n + len > IN_CAP) {
                        c->closing = 1;
                        return;
                    }
                    c->msg = nm;
                    memcpy(c->msg + c->msg_n, p, len);
                    c->msg_n += len;
                    if (fin) {
                        if (c->msg_op == 0x1 && s->h.on_text)
                            s->h.on_text(s->user, c, (const char *)c->msg, c->msg_n);
                        free(c->msg);
                        c->msg = NULL;
                        c->msg_n = 0;
                    }
                }
            }
        }
        memmove(c->in, c->in + h + 4u + len, n - (h + 4u + len));
        c->in_n -= h + 4u + len;
        if (c->closing)
            return;
    }
}

/* -------------------------------------------------------------- server --- */
ws_server_t *ws_server_start(const char *bind_addr, int port, const char *webroot, const ws_handlers_t *h, void *user)
{
    ws_server_t *s = calloc(1, sizeof *s);
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int one = 1, i;
    if (!s)
        return NULL;
    for (i = 0; i < MAX_CONN; i++)
        s->c[i].fd = -1;
    snprintf(s->root, sizeof s->root, "%s", webroot);
    s->h = *h;
    s->user = user;
    s->lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr ? bind_addr : "127.0.0.1", &a.sin_addr) != 1) {
        fprintf(stderr, "web: bad bind address %s\n", bind_addr);
        free(s);
        return NULL;
    }
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (s->lfd < 0 || bind(s->lfd, (struct sockaddr *)&a, sizeof a) || listen(s->lfd, 16)) {
        fprintf(stderr, "web: cannot listen on %s:%d: %s\n", bind_addr ? bind_addr : "127.0.0.1", port, strerror(errno));
        if (s->lfd >= 0)
            close(s->lfd);
        free(s);
        return NULL;
    }
    getsockname(s->lfd, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    return s;
}

int ws_server_port(const ws_server_t *s) { return s->port; }
void ws_server_add_root(ws_server_t *s, const char *dir) { snprintf(s->root2, sizeof s->root2, "%s", dir); }
void ws_server_set_api(ws_server_t *s, ws_api_fn fn) { s->api = fn; }

void ws_server_poll(ws_server_t *s, int timeout_ms)
{
    struct pollfd p[MAX_CONN + 1];
    int i, n = 0, map[MAX_CONN + 1];
    p[n].fd = s->lfd;
    p[n].events = POLLIN;
    map[n++] = -1;
    for (i = 0; i < MAX_CONN; i++)
        if (s->c[i].fd >= 0) {
            p[n].fd = s->c[i].fd;
            p[n].events = POLLIN | (s->c[i].out_n > s->c[i].out_off ? POLLOUT : 0);
            map[n++] = i;
        }
    if (poll(p, (nfds_t)n, timeout_ms) <= 0)
        return;
    if (p[0].revents & POLLIN) {
        int fd = accept4(s->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            for (i = 0; i < MAX_CONN && s->c[i].fd >= 0; i++)
                ;
            if (i == MAX_CONN) {
                close(fd);
            } else {
                memset(&s->c[i], 0, sizeof s->c[i]);
                s->c[i].fd = fd;
                s->c[i].in = malloc(IN_CAP);
            }
        }
    }
    for (i = 1; i < n; i++) {
        ws_conn_t *c = &s->c[map[i]];
        if (c->fd < 0)
            continue;
        if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            conn_close(s, c);
            continue;
        }
        if (p[i].revents & POLLOUT)
            flush(c);
        if (p[i].revents & POLLIN) {
            ssize_t r = recv(c->fd, c->in + c->in_n, IN_CAP - c->in_n, 0);
            if (r <= 0) {
                conn_close(s, c);
                continue;
            }
            c->in_n += (size_t)r;
            if (c->ws)
                ws_input(s, c);
            else
                http_request(s, c);
            if (c->in_n == IN_CAP)
                c->closing = 1;
        }
        if (c->closing && c->out_n == c->out_off)
            conn_close(s, c);
    }
}

void ws_server_stop(ws_server_t *s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < MAX_CONN; i++)
        conn_close(s, &s->c[i]);
    close(s->lfd);
    free(s);
}
