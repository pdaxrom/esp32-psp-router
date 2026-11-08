#pragma once
#include <stdbool.h>
#include <stddef.h>
/* A complete HTTP/1.x 204 response header, not a captive portal/redirect. */
bool status_probe_response(const char *data, size_t length);
