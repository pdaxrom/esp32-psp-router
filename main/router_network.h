#pragma once
#include "esp_netif.h"
typedef struct {
    esp_netif_ip_info_t lan, wan;
    esp_ip4_addr_t dns;
    uint32_t epoch;
    bool online;
} router_network_state_t;
void router_network_init(esp_netif_t *wan, esp_netif_t *lan);
void router_network_poll(bool link_up);
router_network_state_t router_network_state(void);
