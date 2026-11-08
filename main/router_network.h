#pragma once
#include "esp_netif.h"
typedef struct {
    esp_netif_ip_info_t lan, wan;
    esp_ip4_addr_t dns;
    uint32_t epoch;
    bool online;
} router_network_state_t;
#define ROUTER_MAX_WIFI_CLIENTS 4
typedef struct {
    uint8_t mac[6];
    esp_ip4_addr_t dhcp_ip, arp_ip;
    int rssi;
} router_wifi_client_t;
typedef struct {
    bool up, dhcp_running, clients_valid, dhcp_valid, arp_valid;
    unsigned count;
    router_wifi_client_t clients[ROUTER_MAX_WIFI_CLIENTS];
} router_lan_status_t;
void router_network_init(esp_netif_t *wan, esp_netif_t *lan);
void router_network_poll(bool link_up);
router_network_state_t router_network_state(void);
router_lan_status_t router_network_lan_status(void);
