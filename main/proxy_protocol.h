#pragma once
#include <stdbool.h>
#include <stddef.h>
#define PROXY_HEADER_MAX 4096
#define PROXY_URL_MAX 2048
typedef struct {
    bool head, local;
    unsigned port;
    char url[PROXY_URL_MAX], host[256], path[PROXY_URL_MAX];
    char user_agent[256], accept[256];
} proxy_request_t;
/* Returns zero, or an HTTP error status. Mutates the header buffer. */
int proxy_parse_request(char *header, size_t length, proxy_request_t *request);
int proxy_https_url(const char *input, proxy_request_t *request);
bool proxy_decode_query(const char *input, char *output, size_t capacity);
typedef struct { char pending[8]; size_t used; } proxy_rewriter_t;
/* output capacity must be at least length + 8; finish with rewrite_finish. */
size_t proxy_rewrite(proxy_rewriter_t *state, const char *input, size_t length, char *output);
size_t proxy_rewrite_finish(proxy_rewriter_t *state, char *output);
