#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "router_policy.h"
#include "proxy_protocol.h"

static int parse(const char *input, proxy_request_t *r)
{
    char buffer[PROXY_HEADER_MAX + 1];
    size_t size = strlen(input);
    assert(size <= PROXY_HEADER_MAX);
    memcpy(buffer, input, size + 1);
    return proxy_parse_request(buffer, size, r);
}

int main(void)
{
    uint32_t gateway;
    assert(router_choose_lan(0, 0, &gateway) && gateway == 0xc0a83201);
    assert(router_choose_lan(0xc0a80206, 0xffffff00, &gateway) && gateway == 0xc0a83201);
    assert(router_choose_lan(0xc0a83206, 0xffffff00, &gateway) && gateway == 0xc0a83301);
    assert(router_choose_lan(0xc0a83206, 0xffff0000, &gateway) && gateway == 0xac1f3201);
    assert(router_choose_lan(0x0a010101, 0xff000000, &gateway) && gateway == 0xc0a83201);
    assert(!router_choose_lan(1, 0, &gateway));
    assert(!router_choose_lan(1, 0xff00ff00, &gateway));
    assert(!router_choose_lan(1, 0xffffff00, NULL));
    assert(router_client_allowed(0xc0a83202, 0xc0a83201, 0xffffff00, 0xc0a80201));
    assert(router_client_allowed(0xc0a80201, 0xc0a83201, 0xffffff00, 0xc0a80201));
    assert(!router_client_allowed(0xc0a80202, 0xc0a83201, 0xffffff00, 0xc0a80201));
    assert(!router_client_allowed(0x08080808, 0xc0a83201, 0xffffff00, 0xc0a80201));
    proxy_request_t r;
    assert(!parse("GET http://example.com/a?b=1 HTTP/1.0\r\nHost: example.com\r\n\r\n", &r));
    assert(!strcmp(r.url, "https://example.com/a?b=1"));
    assert(!parse("HEAD http://example.com:80?b=1 HTTP/1.1\r\n\r\n", &r));
    assert(r.head && !strcmp(r.url, "https://example.com/?b=1"));
    assert(!parse("GET / HTTP/1.0\r\n\r\n", &r) && r.local);
    assert(!parse("GET /a HTTP/1.1\r\nHost: example.com\r\n\r\n", &r));
    assert(!r.local && !strcmp(r.url, "https://example.com/a"));
    assert(!parse("GET /a HTTP/1.1\r\nHost: example.com \t\r\nX_PSP.Header: ok\r\nContent-Length: 000\r\n\r\n", &r));
    assert(parse("CONNECT example.com:443 HTTP/1.0\r\n\r\n", &r) == 405);
    assert(parse("POST http://example.com/ HTTP/1.0\r\n\r\n", &r) == 405);
    assert(parse("GET http://user@host/ HTTP/1.0\r\n\r\n", &r) == 400);
    assert(parse("GET http://host:65536/ HTTP/1.0\r\n\r\n", &r) == 400);
    assert(parse("GET http://host/ HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 400);
    assert(parse("GET http://host/ HTTP/1.0\r\nContent-Length: 1\r\n\r\n", &r) == 400);
    assert(parse("GET http://host/ HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", &r) == 400);
    assert(parse("GET http://host/ HTTP/1.0\r\n Host: a\r\n\r\n", &r) == 400);
    assert(parse("GET http://host/ HTTP/2.0\r\n\r\n", &r) == 505);
    assert(proxy_https_url("https://host:8443/path#fragment", &r) == 0);
    assert(!strcmp(r.url, "https://host:8443/path"));
    char decoded[256];
    assert(proxy_decode_query("https%3A%2F%2Fhost%2Fa%3Fx%3D%2520", decoded, sizeof(decoded)));
    assert(!strcmp(decoded, "https://host/a?x=%20"));
    assert(!proxy_decode_query("x%00y", decoded, sizeof(decoded)));
    assert(!proxy_decode_query("x%0d%0Ay", decoded, sizeof(decoded)));
    assert(!proxy_decode_query("x%", decoded, sizeof(decoded)));
    assert(!proxy_decode_query("abc", decoded, 3));
    const char *input = "<a href='HTTPS://a/'>https://b/</a> httX https:/";
    const char *expected = "<a href='http://a/'>http://b/</a> httX https:/";
    for (size_t split = 0; split <= strlen(input); ++split) {
        proxy_rewriter_t state = {0};
        char output[256];
        size_t n = proxy_rewrite(&state, input, split, output);
        n += proxy_rewrite(&state, input + split, strlen(input) - split, output + n);
        n += proxy_rewrite_finish(&state, output + n);
        output[n] = 0;
        assert(!strcmp(output, expected));
    }
    puts("PASS: LAN subnet selection, HTTP parser rejection cases, URL decoding, streaming link rewrites at every boundary");
}
