#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

typedef struct {
    bool enabled, usb, configured, internet;
    unsigned clients;
    uint32_t generation;
} status_rgb_state_t;

void status_rgb_init(esp_netif_t *internet_netif);
/* Main-loop update and Wi-Fi event update never perform network probes/RMT TX. */
void status_rgb_poll(bool usb, uint32_t usb_epoch);
void status_rgb_station(const uint8_t mac[6], bool connected);
status_rgb_state_t status_rgb_state(void);
