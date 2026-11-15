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

proxy_admin_route_t proxy_admin_route(const char *path)
{
    if (!strcmp(path, "/admin") || !strcmp(path, "/admin/") ||
        !strcmp(path, "/admin?") || !strcmp(path, "/admin/?")) return PROXY_ADMIN_PAGE;
    if (!strcmp(path, "/admin/login")) return PROXY_ADMIN_LOGIN;
    if (!strcmp(path, "/admin/logout")) return PROXY_ADMIN_LOGOUT;
    if (!strcmp(path, "/admin/wifi")) return PROXY_ADMIN_WIFI;
    if (!strcmp(path, "/admin/password")) return PROXY_ADMIN_PASSWORD;
    if (!strcmp(path, "/admin/bootloader")) return PROXY_ADMIN_BOOTLOADER;
    return PROXY_ADMIN_NONE;
}

bool proxy_admin_cookie(const char *header, char token[PROXY_ADMIN_TOKEN_LENGTH + 1])
{
    static const char name[] = "psp_admin=";
    bool found = false;
    while (header && *header) {
        header += strspn(header, " \t");
        const char *end = header + strcspn(header, ";");
        if ((size_t)(end - header) >= sizeof(name) - 1 && !strncmp(header, name, sizeof(name) - 1)) {
            const char *value = header + sizeof(name) - 1;
            const char *value_end = end;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
            if (found || value_end - value != PROXY_ADMIN_TOKEN_LENGTH ||
                strspn(value, "0123456789abcdef") != PROXY_ADMIN_TOKEN_LENGTH) return false;
            memcpy(token, value, PROXY_ADMIN_TOKEN_LENGTH);
            token[PROXY_ADMIN_TOKEN_LENGTH] = '\0';
            found = true;
        }
        header = *end ? end + 1 : end;
    }
    return found;
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
    header[length - 2] = 0; /* Terminate the header without touching any buffered body. */
    char *line_end = strstr(header, "\r\n");
    if (!line_end) return 400;
    *line_end = 0;
    char *target = strchr(header, ' ');
    if (!target) return 400;
    *target++ = 0;
    r->head = !strcmp(header, "HEAD");
    r->post = !strcmp(header, "POST");
    if (!r->head && !r->post && strcmp(header, "GET")) return 405;
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
    if (r->post && proxy_admin_route(r->path) < PROXY_ADMIN_LOGIN) return 405;
    bool content_length = false, duplicate_cookie = false;
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
            if (r->host_header || !*colon) return 400;
            r->host_header = colon;
            if (r->local && !copy(r->host, sizeof(r->host), colon)) return 431;
        }
        if (!strcasecmp(line, "Content-Length")) {
            if (content_length || !*colon) return 400;
            content_length = true;
            for (const char *digit = colon; *digit; ++digit) {
                if (*digit < '0' || *digit > '9') return 400;
                if (!r->post && *digit != '0') return 400;
                r->content_length = r->content_length * 10 + (*digit - '0');
                if (r->content_length > PROXY_ADMIN_BODY_MAX) return 413;
            }
        }
        if (!strcasecmp(line, "Cookie")) {
            if (r->cookie) duplicate_cookie = true;
            r->cookie = colon;
        }
        if (r->post && !strcasecmp(line, "Content-Type")) {
            if (r->content_type) return 400;
            r->content_type = colon;
        }
        if (!strcasecmp(line, "User-Agent") && !copy(r->user_agent, sizeof(r->user_agent), colon)) return 431;
        if (!strcasecmp(line, "Accept") && !copy(r->accept, sizeof(r->accept), colon)) return 431;
        line = next + 2;
    }
    if (duplicate_cookie) r->cookie = NULL;
    if (r->post && !content_length) return 411;
    if (r->post && !r->content_length) return 400;
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

static bool decode(const char *input, char *output, size_t capacity, bool allow_space)
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
        if (c < 32 || (c == 32 && !allow_space) || c == 127 || used + 1 >= capacity) return false;
        output[used++] = c;
    }
    if (!capacity) return false;
    output[used] = 0;
    return true;
}

bool proxy_decode_query(const char *input, char *output, size_t capacity)
{
    return decode(input, output, capacity, false);
}

bool proxy_parse_form(char *body, const proxy_form_field_t *fields, size_t count)
{
    unsigned seen = 0;
    if (count > 8) return false;
    while (*body) {
        char *next = strchr(body, '&');
        if (next) *next++ = '\0';
        char *value = strchr(body, '=');
        if (!value) return false;
        *value++ = '\0';
        size_t index = 0;
        while (index < count && strcmp(body, fields[index].name)) ++index;
        if (index == count || (seen & (1u << index))) return false;
        if (!decode(value, fields[index].value, fields[index].capacity, true)) return false;
        seen |= 1u << index;
        if (!next) break;
        if (!*next) return false;
        body = next;
    }
    return seen == (1u << count) - 1;
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
