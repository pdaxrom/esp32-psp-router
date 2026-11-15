#pragma once

#include "proxy_protocol.h"

typedef enum {
    WEB_ADMIN_ACTION_NONE, WEB_ADMIN_APPLY_WIFI, WEB_ADMIN_RESTART_ROM
} web_admin_action_t;

typedef struct {
    int status;
    web_admin_action_t action;
    const char *location;
    char cookie[PROXY_ADMIN_TOKEN_LENGTH + 1];
    bool clear_cookie;
    char html[2800];
} web_admin_reply_t;

void web_admin_init(void);
void web_admin_apply_pending_wifi(void);
void web_admin_handle(const proxy_request_t *request, proxy_admin_route_t route,
                      char *body, web_admin_reply_t *reply);
