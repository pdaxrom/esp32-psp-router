#include <string.h>
#include "sdkconfig.h"
#if CONFIG_PSP_MODE_ROUTER
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/def.h"
#if CONFIG_PSP_AP_WPA2
#include "esp_netif_net_stack.h"
#include "lwip/etharp.h"
#include "lwip/ip4.h"
#endif
#include "dhcpserver/dhcpserver.h"
#include "router_network.h"
#include "router_policy.h"

static esp_netif_t *s_wan, *s_lan;
static router_network_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_napt;

#if CONFIG_PSP_AP_WPA2
/* Keep DHCP-derived neighbors for associated WPA2 stations. Some legacy
 * clients do not answer the AP's broadcast ARP reliably after association.
 * IDF removes its temporary static neighbor after sending each DHCP reply.
 * All ARP access below runs in TCP/IP context, never in the Wi-Fi task.
 */
typedef struct {
    ip4_addr_t ip;
    struct eth_addr mac;
} lan_neighbor_t;
typedef struct {
    unsigned count;
    lan_neighbor_t neighbors[ROUTER_MAX_WIFI_CLIENTS];
} lan_neighbors_t;
static lan_neighbors_t s_neighbors;

static bool same_neighbor(const lan_neighbor_t *a, const lan_neighbor_t *b)
{
    return ip4_addr_eq(&a->ip, &b->ip) && !memcmp(&a->mac, &b->mac, sizeof(a->mac));
}

static bool owns_arp_entry(struct netif *lan, const lan_neighbor_t *neighbor)
{
    for (size_t i = 0; i < ARP_TABLE_SIZE; ++i) {
        ip4_addr_t *ip;
        struct netif *netif;
        struct eth_addr *mac;
        if (etharp_get_entry(i, &ip, &netif, &mac) && netif == lan &&
            ip4_addr_eq(ip, &neighbor->ip) && !memcmp(mac, &neighbor->mac, sizeof(*mac))) {
            return true;
        }
    }
    return false;
}

static esp_err_t sync_lan_neighbors(void *arg)
{
    const lan_neighbors_t *wanted = arg;
    lan_neighbors_t valid = {0};
    struct netif *lan = esp_netif_get_netif_impl(s_lan);
    if (!lan) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < wanted->count; ++i) {
        const lan_neighbor_t *neighbor = &wanted->neighbors[i];
        if (!netif_is_up(lan) || ip4_addr_isany_val(neighbor->ip) ||
            ip4_addr_eq(&neighbor->ip, netif_ip4_addr(lan)) ||
            !ip4_addr_netcmp(&neighbor->ip, netif_ip4_addr(lan), netif_ip4_netmask(lan)) ||
            ip4_addr_isbroadcast(&neighbor->ip, lan) ||
            ip4_route(&neighbor->ip) != lan || (neighbor->mac.addr[0] & 1)) continue;
        valid.neighbors[valid.count++] = *neighbor;
    }
    for (unsigned i = 0; i < s_neighbors.count; ++i) {
        const lan_neighbor_t *old = &s_neighbors.neighbors[i];
        bool retained = false;
        for (unsigned j = 0; j < valid.count; ++j) {
            retained |= same_neighbor(old, &valid.neighbors[j]);
        }
        if (!retained && owns_arp_entry(lan, old)) {
            /* remove_static_entry leaves any dynamic replacement untouched. */
            etharp_remove_static_entry(&old->ip);
        }
    }
    s_neighbors = (lan_neighbors_t){0};
    esp_err_t result = ESP_OK;
    for (unsigned i = 0; i < valid.count; ++i) {
        lan_neighbor_t *neighbor = &valid.neighbors[i];
        if (etharp_add_static_entry(&neighbor->ip, &neighbor->mac) == ERR_OK) {
            s_neighbors.neighbors[s_neighbors.count++] = *neighbor;
        } else result = ESP_FAIL; /* Retry next poll; no restart or security fallback. */
    }
    return result;
}

static void maintain_lan_neighbors(void)
{
    static TickType_t previous;
    TickType_t now = xTaskGetTickCount();
    if (now - previous < pdMS_TO_TICKS(250)) return;
    previous = now;
    wifi_sta_list_t stations = {0};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return;
    unsigned count = stations.num > ROUTER_MAX_WIFI_CLIENTS ? ROUTER_MAX_WIFI_CLIENTS : stations.num;
    esp_netif_pair_mac_ip_t leases[ROUTER_MAX_WIFI_CLIENTS] = {0};
    for (unsigned i = 0; i < count; ++i) memcpy(leases[i].mac, stations.sta[i].mac, 6);
    if (count && esp_netif_dhcps_get_clients_by_mac(s_lan, count, leases) != ESP_OK) return;
    lan_neighbors_t wanted = {0};
    for (unsigned i = 0; i < count; ++i) {
        if (!leases[i].ip.addr) continue;
        lan_neighbor_t *neighbor = &wanted.neighbors[wanted.count++];
        neighbor->ip.addr = leases[i].ip.addr;
        memcpy(neighbor->mac.addr, leases[i].mac, sizeof(neighbor->mac.addr));
    }
    (void)esp_netif_tcpip_exec(sync_lan_neighbors, &wanted);
}
#endif

router_network_state_t router_network_state(void)
{
    portENTER_CRITICAL(&s_lock);
    router_network_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

router_lan_status_t router_network_lan_status(void)
{
    router_lan_status_t status = {0};
    if (!s_lan) return status;
    status.up = esp_netif_is_netif_up(s_lan);
    esp_netif_dhcp_status_t dhcp;
    status.dhcp_running = esp_netif_dhcps_get_status(s_lan, &dhcp) == ESP_OK &&
                          dhcp == ESP_NETIF_DHCP_STARTED;
    wifi_sta_list_t stations = {0};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return status;
    status.clients_valid = true;
    status.count = stations.num > ROUTER_MAX_WIFI_CLIENTS ? ROUTER_MAX_WIFI_CLIENTS : stations.num;
    esp_netif_pair_mac_ip_t leases[ROUTER_MAX_WIFI_CLIENTS] = {0};
    for (unsigned i = 0; i < status.count; ++i) {
        memcpy(leases[i].mac, stations.sta[i].mac, sizeof(leases[i].mac));
        memcpy(status.clients[i].mac, stations.sta[i].mac, sizeof(status.clients[i].mac));
        status.clients[i].rssi = stations.sta[i].rssi;
    }
    status.dhcp_valid = !status.count ||
        esp_netif_dhcps_get_clients_by_mac(s_lan, status.count, leases) == ESP_OK;
    status.arp_valid = true;
    for (unsigned i = 0; i < status.count; ++i) {
        if (status.dhcp_valid) status.clients[i].dhcp_ip = leases[i].ip;
        esp_netif_pair_mac_ip_t arp = {0};
        memcpy(arp.mac, stations.sta[i].mac, sizeof(arp.mac));
        if (esp_netif_arp_get_client_by_mac(s_lan, &arp) == ESP_OK) {
            status.clients[i].arp_ip = arp.ip;
        } else {
            status.arp_valid = false;
        }
    }
    return status;
}

static void publish(router_network_state_t *state)
{
    ++state->epoch;
    portENTER_CRITICAL(&s_lock);
    s_state = *state;
    portEXIT_CRITICAL(&s_lock);
}

static void configure_lan(esp_netif_ip_info_t *ip)
{
#if CONFIG_PSP_AP_WPA2
    const lan_neighbors_t empty = {0};
    ESP_ERROR_CHECK(esp_netif_tcpip_exec(sync_lan_neighbors, (void *)&empty));
#endif
    esp_netif_dhcps_stop(s_lan);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_lan, ip));
    esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4, .u_addr.ip4 = ip->ip}};
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_lan, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_lan, ESP_NETIF_OP_SET,
                                        ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_lan));
}

void router_network_init(esp_netif_t *wan, esp_netif_t *lan)
{
    s_wan = wan;
    s_lan = lan;
    router_network_state_t state = {.epoch = 0};
    state.lan.ip.addr = PP_HTONL(0xc0a83201); /* 192.168.50.1/24 */
    state.lan.gw = state.lan.ip;
    state.lan.netmask.addr = PP_HTONL(0xffffff00);
    configure_lan(&state.lan);
    publish(&state);
#if CONFIG_PSP_HTTP_PROXY
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                       ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    ntp.start = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
#endif
}

void router_network_poll(bool link_up)
{
#if CONFIG_PSP_AP_WPA2
    maintain_lan_neighbors();
#endif
    router_network_state_t next = router_network_state();
    esp_netif_ip_info_t wan = {0};
    esp_netif_dns_info_t dns = {0};
    if (link_up) {
        esp_netif_get_ip_info(s_wan, &wan);
        esp_netif_get_dns_info(s_wan, ESP_NETIF_DNS_MAIN, &dns);
    }
    bool have_ip = link_up && wan.ip.addr && wan.netmask.addr && wan.gw.addr;
    esp_ip4_addr_t resolver = {0};
    if (dns.ip.type == ESP_IPADDR_TYPE_V4) {
        resolver = dns.ip.u_addr.ip4;
    }
    bool same_wan = memcmp(&next.wan, &wan, sizeof(wan)) == 0;
    if (same_wan && next.dns.addr == resolver.addr && next.online == (have_ip && s_napt)) {
        return;
    }
    /* Disable before every WAN change: lwIP frees all stale NAPT mappings. */
    if (s_napt) {
        ESP_ERROR_CHECK(esp_netif_napt_disable(s_lan));
        s_napt = false;
    }
    uint32_t gateway;
    if (have_ip && router_choose_lan(lwip_ntohl(wan.ip.addr), lwip_ntohl(wan.netmask.addr), &gateway)) {
        if (next.lan.ip.addr != lwip_htonl(gateway)) {
            next.lan.ip.addr = lwip_htonl(gateway);
            next.lan.gw = next.lan.ip;
            configure_lan(&next.lan);
            esp_wifi_deauth_sta(0); /* New subnet requires fresh client leases. */
        }
        ESP_ERROR_CHECK(esp_netif_set_default_netif(s_wan));
        ESP_ERROR_CHECK(esp_netif_napt_enable(s_lan));
        s_napt = true;
#if CONFIG_PSP_HTTP_PROXY
        ESP_ERROR_CHECK(esp_netif_sntp_start());
#endif
    }
    next.wan = wan;
    next.dns = resolver;
    next.online = have_ip && s_napt;
    publish(&next);
}
#endif
