#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_netif.h"
void bridge_proxy_init(esp_netif_t *bridge, esp_netif_t *ap);
void bridge_proxy_poll(bool link_up);
void bridge_proxy_station_event(const uint8_t mac[6], bool connected);
/* Called by USB TX. Does not retain input, block, or call the USB stack. */
bool bridge_proxy_intercept(const void *frame, size_t length);
