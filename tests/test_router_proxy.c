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
    assert(!parse("POST /admin/wifi HTTP/1.1\r\nHost: psp-proxy:8080\r\n"
                  "Content-Length: 32\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                  "Cookie: psp_admin=0123456789abcdef0123456789abcdef\r\n\r\n", &r));
    assert(r.post && r.port == 8080 && r.content_length == 32 && r.cookie);
    assert(proxy_admin_route(r.path) == PROXY_ADMIN_WIFI);
    char buffered[] = "POST /admin/wifi HTTP/1.1\r\nHost: psp-proxy:8080\r\nContent-Length: 6\r\n\r\nssid=x";
    size_t header_length = (size_t)(strstr(buffered, "\r\n\r\n") - buffered) + 4;
    assert(!proxy_parse_request(buffered, header_length, &r));
    assert(r.content_length == 6 && !strcmp(buffered + header_length, "ssid=x"));
    assert(!parse("POST /admin/bootloader HTTP/1.1\r\nHost: psp-proxy:8080\r\nContent-Length: 38\r\n\r\n", &r));
    assert(r.post && r.content_length == 38 && proxy_admin_route(r.path) == PROXY_ADMIN_BOOTLOADER);
    assert(parse("POST /admin/password HTTP/1.0\r\n\r\n", &r) == 411);
    assert(parse("POST /admin/wifi HTTP/1.0\r\nContent-Length: 513\r\n\r\n", &r) == 413);
    assert(parse("POST /admin/wifi HTTP/1.0\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n", &r) == 400);
    assert(parse("POST /admin/wifi HTTP/1.0\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == 400);
    assert(parse("POST /admin/ HTTP/1.0\r\nContent-Length: 5\r\n\r\n", &r) == 405);
    assert(!parse("GET http://psp-proxy:8080/admin/? HTTP/1.0\r\n\r\n", &r));
    assert(proxy_admin_route(r.path) == PROXY_ADMIN_PAGE);
    assert(proxy_admin_route("/admin?") == PROXY_ADMIN_PAGE);
    assert(proxy_admin_route("/admin/?extra=1") == PROXY_ADMIN_NONE);
    assert(parse("POST /admin/logout? HTTP/1.0\r\nContent-Length: 5\r\n\r\n", &r) == 405);
    assert(!parse("POST /admin/logout HTTP/1.0\r\nContent-Length: 38\r\n\r\n", &r));
    assert(r.post && proxy_admin_route(r.path) == PROXY_ADMIN_LOGOUT);
    assert(!parse("GET /admin/ HTTP/1.0\r\nCookie: a=1\r\nCookie: b=1\r\n\r\n", &r));
    assert(!r.cookie);
    char token[PROXY_ADMIN_TOKEN_LENGTH + 1];
    assert(proxy_admin_cookie("a=1; psp_admin=0123456789abcdef0123456789abcdef; b=2", token));
    assert(!strcmp(token, "0123456789abcdef0123456789abcdef"));
    assert(!proxy_admin_cookie("xpsp_admin=0123456789abcdef0123456789abcdef", token));
    assert(!proxy_admin_cookie("psp_admin=0123456789abcdef0123456789abcdef; psp_admin=aa", token));
    assert(!proxy_admin_cookie("psp_admin=0123456789abcdef0123456789abcdeg", token));
    char ssid[33], password[64];
    const proxy_form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}, {"password", password, sizeof(password)}};
    char form[] = "password=a%26b%3Dc%2Bde&ssid=PSP+room";
    assert(proxy_parse_form(form, fields, 2));
    assert(!strcmp(ssid, "PSP room") && !strcmp(password, "a&b=c+de"));
    const char *invalid_forms[] = {"ssid=x&ssid=y", "ssid=x&password=%00", "ssid=x&password=%0A", "ssid=x&password=y&",
                                   "ssid=x&password=y&surprise=z", "ssid=x", "ssid=x&password"};
    for (size_t i = 0; i < sizeof(invalid_forms) / sizeof(invalid_forms[0]); ++i) {
        char body[128];
        strcpy(body, invalid_forms[i]);
        assert(!proxy_parse_form(body, fields, 2));
    }
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
    puts("PASS: LAN subnet selection, HTTP parser and admin form boundaries, URL decoding, streaming link rewrites");
}
