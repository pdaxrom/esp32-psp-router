#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSP_PROXY_HOSTNAME "psp-proxy"
#define BRIDGE_DNS_FRAME_MAX 576

bool bridge_dns_hostname(const char *name);
/* IPv4 addresses are host byte order. NULL output validates and returns size.
 * Zero means leave the query/frame untouched; only our exact name is owned.
 */
size_t bridge_dns_message_reply(const uint8_t *query, size_t length, uint32_t ip,
                                uint8_t *reply, size_t capacity);
size_t bridge_dns_frame_reply(const uint8_t *frame, size_t length, uint32_t ip,
                              uint32_t mask, uint8_t *reply, size_t capacity);
