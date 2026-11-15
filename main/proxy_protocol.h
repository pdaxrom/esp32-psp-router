#pragma once
#include <stdbool.h>
#include <stddef.h>
#define PROXY_HEADER_MAX 4096
#define PROXY_URL_MAX 2048
#define PROXY_ADMIN_BODY_MAX 512
#define PROXY_ADMIN_TOKEN_LENGTH 32
typedef enum {
    PROXY_ADMIN_NONE, PROXY_ADMIN_PAGE, PROXY_ADMIN_LOGIN, PROXY_ADMIN_LOGOUT,
    PROXY_ADMIN_WIFI, PROXY_ADMIN_PASSWORD, PROXY_ADMIN_BOOTLOADER
} proxy_admin_route_t;
typedef struct {
    bool head, local, post;
    unsigned port;
    size_t content_length;
    const char *host_header, *cookie, *content_type; /* Borrowed from header. */
    char url[PROXY_URL_MAX], host[256], path[PROXY_URL_MAX];
    char user_agent[256], accept[256];
} proxy_request_t;
/* Returns zero, or an HTTP error status. Mutates the header buffer. */
int proxy_parse_request(char *header, size_t length, proxy_request_t *request);
int proxy_https_url(const char *input, proxy_request_t *request);
bool proxy_decode_query(const char *input, char *output, size_t capacity);
proxy_admin_route_t proxy_admin_route(const char *path);
bool proxy_admin_cookie(const char *header, char token[PROXY_ADMIN_TOKEN_LENGTH + 1]);
typedef struct {
    const char *name;
    char *value;
    size_t capacity;
} proxy_form_field_t;
bool proxy_parse_form(char *body, const proxy_form_field_t *fields, size_t count);
typedef struct { char pending[8]; size_t used; } proxy_rewriter_t;
/* output capacity must be at least length + 8; finish with rewrite_finish. */
size_t proxy_rewrite(proxy_rewriter_t *state, const char *input, size_t length, char *output);
size_t proxy_rewrite_finish(proxy_rewriter_t *state, char *output);
