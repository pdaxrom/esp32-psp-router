#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "proxy_protocol.h"

static bool copy(char *to, size_t capacity, const char *from)
{
    size_t size = strlen(from);
    if (size >= capacity) return false;
    memcpy(to, from, size + 1);
    return true;
}

static bool token(unsigned char c)
{
    return isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

int proxy_https_url(const char *input, proxy_request_t *r)
{
    const char *authority;
    bool http = !strncasecmp(input, "http://", 7);
    if (http) authority = input + 7;
    else if (!strncasecmp(input, "https://", 8)) authority = input + 8;
    else return 400;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        if (*p <= 32 || *p == 127 || *p == '\\') return 400;
    }
    const char *end = authority + strcspn(authority, "/?#");
    const char *colon = memchr(authority, ':', end - authority);
    size_t host_size = (colon ? colon : end) - authority;
    if (!host_size || host_size >= sizeof(r->host)) return 400;
    for (size_t i = 0; i < host_size; ++i) {
        unsigned char c = authority[i];
        if (!(isalnum(c) || c == '-' || c == '.')) return 400;
    }
    memcpy(r->host, authority, host_size);
    r->host[host_size] = 0;
    if (r->host[0] == '.' || r->host[host_size - 1] == '.') return 400;
    unsigned port = 443;
    if (colon) {
        if (colon + 1 == end) return 400;
        port = 0;
        for (const char *p = colon + 1; p < end; ++p) {
            if (*p < '0' || *p > '9' || port > 6553) return 400;
            port = port * 10 + *p - '0';
        }
        if (!port || port > 65535) return 400;
    }
    bool default_http_port = http && colon && port == 80;
    if (default_http_port) port = 443;
    r->port = port;
    const char *suffix = *end == '#' ? "" : end;
    size_t path_size = strcspn(suffix, "#");
    bool slash = !path_size || suffix[0] != '/';
    if (path_size + slash >= sizeof(r->path)) return 414;
    r->path[0] = '/';
    memcpy(r->path + slash, suffix, path_size);
    r->path[path_size + slash] = 0;
    char explicit_port[8] = "";
    if (colon && !default_http_port) snprintf(explicit_port, sizeof(explicit_port), ":%u", port);
    int n = snprintf(r->url, sizeof(r->url), "https://%s%s%s", r->host, explicit_port, r->path);
    return n < 0 || (size_t)n >= sizeof(r->url) ? 414 : 0;
}

int proxy_parse_request(char *header, size_t length, proxy_request_t *r)
{
    if (!r || length < 4 || length > PROXY_HEADER_MAX || memchr(header, 0, length) ||
        memcmp(header + length - 4, "\r\n\r\n", 4)) return 400;
    memset(r, 0, sizeof(*r));
    header[length] = 0;
    header[length - 2] = 0; /* Empty line terminates the header iterator. */
    char *line_end = strstr(header, "\r\n");
    if (!line_end) return 400;
    *line_end = 0;
    char *target = strchr(header, ' ');
    if (!target) return 400;
    *target++ = 0;
    if (strcmp(header, "GET") && strcmp(header, "HEAD")) return 405;
    r->head = !strcmp(header, "HEAD");
    char *version = strchr(target, ' ');
    if (!version) return 400;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) return 505;
    if (*target == '/') {
        r->local = true;
        if (!copy(r->path, sizeof(r->path), target)) return 414;
    } else {
        int error = proxy_https_url(target, r);
        if (error) return error;
    }
    bool content_length = false, host = false;
    for (char *line = line_end + 2; *line;) {
        char *next = strstr(line, "\r\n");
        if (!next) return 400;
        *next = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return 400;
        for (char *p = line; p < colon; ++p) {
            if (!token((unsigned char)*p)) return 400;
        }
        *colon++ = 0;
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *value_end = colon + strlen(colon);
        while (value_end > colon && (value_end[-1] == ' ' || value_end[-1] == '\t')) *--value_end = 0;
        for (char *p = colon; *p; ++p) if (((unsigned char)*p < 32 && *p != '\t') || *p == 127) return 400;
        if (!strcasecmp(line, "Transfer-Encoding")) return 400;
        if (!strcasecmp(line, "Host")) {
            if (host || !*colon) return 400;
            host = true;
            if (r->local && !copy(r->host, sizeof(r->host), colon)) return 431;
        }
        if (!strcasecmp(line, "Content-Length")) {
            if (content_length || !*colon || strspn(colon, "0") != strlen(colon)) return 400;
            content_length = true;
        }
        if (!strcasecmp(line, "User-Agent") && !copy(r->user_agent, sizeof(r->user_agent), colon)) return 431;
        if (!strcasecmp(line, "Accept") && !copy(r->accept, sizeof(r->accept), colon)) return 431;
        line = next + 2;
    }
    if (r->local && r->host[0]) {
        int n = snprintf(r->url, sizeof(r->url), "http://%s%s", r->host, r->path);
        if (n < 0 || (size_t)n >= sizeof(r->url)) return 414;
        int error = proxy_https_url(r->url, r);
        if (error) return error;
        r->local = false; /* Host lets HTTP/1.1 origin-form requests use the proxy. */
    }
    return 0;
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool proxy_decode_query(const char *input, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; input[i]; ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (!input[i + 1] || !input[i + 2]) return false;
            int a = hex(input[i + 1]), b = hex(input[i + 2]);
            if (a < 0 || b < 0) return false;
            c = (a << 4) | b;
            i += 2;
        } else if (c == '+') c = ' ';
        if (c <= 32 || c == 127 || used + 1 >= capacity) return false;
        output[used++] = c;
    }
    if (!capacity) return false;
    output[used] = 0;
    return true;
}

size_t proxy_rewrite(proxy_rewriter_t *s, const char *input, size_t length, char *output)
{
    static const char pattern[] = "https://";
    size_t out = 0;
    for (size_t i = 0; i < length; ++i) {
        s->pending[s->used++] = input[i];
        for (;;) {
            bool prefix = true;
            for (size_t j = 0; j < s->used; ++j) {
                if (tolower((unsigned char)s->pending[j]) != pattern[j]) prefix = false;
            }
            if (prefix) break;
            output[out++] = s->pending[0];
            --s->used;
            memmove(s->pending, s->pending + 1, s->used);
        }
        if (s->used == 8) {
            memcpy(output + out, "http://", 7);
            out += 7;
            s->used = 0;
        }
    }
    return out;
}

size_t proxy_rewrite_finish(proxy_rewriter_t *s, char *output)
{
    size_t count = s->used;
    memcpy(output, s->pending, count);
    s->used = 0;
    return count;
}
