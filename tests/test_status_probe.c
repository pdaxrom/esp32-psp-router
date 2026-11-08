#include "status_probe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
    const char good[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
    for (size_t n = 0; n < sizeof(good) - 1; ++n) assert(!status_probe_response(good, n));
    assert(status_probe_response(good, sizeof(good) - 1));
    const char minimal[] = "HTTP/1.0 204\r\n\r\n";
    assert(status_probe_response(minimal, sizeof(minimal) - 1));
    const char *invalid[] = {
        "HTTP/1.1 200 OK\r\n\r\nlogin", "HTTP/1.1 302 Redirect\r\n\r\n",
        "HTTP/1.1 503 Unavailable\r\n\r\n", "HTTP/1.1 2040 Bad\r\n\r\n",
        "HTTP/1.1 204X\r\n\r\n", "HTTP/2.0 204\r\n\r\n",
        "HTTP/1.1 103 Hint\r\n\r\nHTTP/1.1 204\r\n\r\n",
        "garbage HTTP/1.1 204\r\n\r\n", "HTTP/1.1 204 No Content\n\n"
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i)
        assert(!status_probe_response(invalid[i], strlen(invalid[i])));
    char response[512];
    memcpy(response, good, sizeof(good)); response[20] = 0;
    assert(!status_probe_response(response, sizeof(good) - 1));
    assert(!status_probe_response(NULL, 512));
    uint32_t seed = 19;
    for (unsigned i = 0; i < 10000; ++i) {
        seed = seed * 1664525 + 1013904223;
        size_t n = seed % sizeof(response);
        memset(response, (seed >> 16) & 255, n);
        if (n >= 12 && (i & 1)) memcpy(response, "HTTP/1.1 204", 12);
        (void)status_probe_response(response, n);
    }
    puts("PASS: HTTP connectivity probe rejects captive portals, redirects, invalid status and incomplete headers");
}
