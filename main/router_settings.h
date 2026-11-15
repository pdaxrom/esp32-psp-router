#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_types.h"

typedef struct {
    char ssid[33];
    char password[64];
} router_wifi_settings_t;

void router_wifi_settings_load(router_wifi_settings_t *settings);
void router_wifi_settings_defaults(router_wifi_settings_t *settings);
esp_err_t router_wifi_ap_start(esp_netif_t *ap);
bool router_wifi_settings_get_active(router_wifi_settings_t *settings);
esp_err_t router_wifi_settings_apply(const router_wifi_settings_t *settings);
