/* SPDX-License-Identifier: GPL-3.0-only
 * sloopy-sloop-32 host test: the host web server's handshake primitives (SHA-1, base64, the
 * RFC 6455 accept key). */
#include <string.h>

#include "sim.h"
#include "ws_server.h"

static void hex(const uint8_t *d, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", d[i]);
}

int main(void)
{
    uint8_t d[20];
    char h[41], b[64], acc[29];
    static char big[1000001];
    printf("ws_server:\n");
    ws_sha1((const uint8_t *)"abc", 3, d);
    hex(d, 20, h);
    sim_check(!strcmp(h, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1(\"abc\")");
    ws_sha1((const uint8_t *)"", 0, d);
    hex(d, 20, h);
    sim_check(!strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "sha1(\"\")");
    {
        const char *s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes: two blocks */
        ws_sha1((const uint8_t *)s, strlen(s), d);
        hex(d, 20, h);
        sim_check(!strcmp(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "sha1(448-bit message)");
    }
    memset(big, 'a', 1000000);
    ws_sha1((const uint8_t *)big, 1000000, d);
    hex(d, 20, h);
    sim_check(!strcmp(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "sha1(a x 1e6)");
    ws_base64((const uint8_t *)"foobar", 6, b, sizeof b);
    sim_check(!strcmp(b, "Zm9vYmFy"), "base64(foobar)");
    ws_base64((const uint8_t *)"fooba", 5, b, sizeof b);
    sim_check(!strcmp(b, "Zm9vYmE="), "base64(fooba)");
    ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc);
    sim_check(!strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "RFC 6455 accept key example");
    printf("ws_server: %s\n", sim_fails ? "FAIL" : "PASS");
    return sim_fails ? 1 : 0;
}
