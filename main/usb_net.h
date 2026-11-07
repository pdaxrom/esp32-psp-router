#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

typedef struct {
    bool active;
    uint32_t epoch;
} usb_net_status_t;

esp_netif_t *usb_net_create(const uint8_t bridge_mac[6]);
void usb_net_start(void);
void usb_net_accept_rx(bool accept);
usb_net_status_t usb_net_status(void);
