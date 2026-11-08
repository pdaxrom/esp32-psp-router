#include "sdkconfig.h"
#if CONFIG_PSP_BRIDGE_PROXY
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/etharp.h"
#include "lwip/ip4.h"
#include "lwip/sockets.h"
#include "bridge_proxy.h"
#include "bridge_dns.h"
#include "router_network.h"
#include "router_policy.h"

static esp_netif_t *s_bridge, *s_ap;
static router_network_state_t s_state = {.bridge_mode = true};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct { uint8_t mac[6]; uint32_t ip; int rssi; bool active; } client_t;
static client_t s_clients[ROUTER_MAX_WIFI_CLIENTS];
static unsigned s_intercepted, s_direct;
typedef struct { uint32_t epoch; size_t len; uint8_t frame[BRIDGE_DNS_FRAME_MAX]; } dns_job_t;
static QueueHandle_t s_dns_queue;

router_network_state_t router_network_state(void)
{
    portENTER_CRITICAL(&s_lock);
    router_network_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

void bridge_proxy_station_event(const uint8_t mac[6], bool connected)
{
    portENTER_CRITICAL(&s_lock);
    int slot = -1;
    for (unsigned i = 0; i < ROUTER_MAX_WIFI_CLIENTS; ++i) {
        if (!memcmp(s_clients[i].mac, mac, 6)) { slot = i; break; }
        if (!s_clients[i].active) slot = i;
    }
    if (slot >= 0) {
        memset(&s_clients[slot], 0, sizeof(s_clients[slot]));
        memcpy(s_clients[slot].mac, mac, 6);
        s_clients[slot].active = connected;
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool learn_client(const uint8_t mac[6], uint32_t ip)
{
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < ROUTER_MAX_WIFI_CLIENTS; ++i) {
        if (s_clients[i].active && !memcmp(s_clients[i].mac, mac, 6)) {
            s_clients[i].ip = ip;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

bool bridge_proxy_intercept(const void *frame, size_t length)
{
    if (!s_dns_queue) return false;
    router_network_state_t state = router_network_state();
    if (!state.online) return false;
    const uint8_t *in = frame;
    size_t size = bridge_dns_frame_reply(in, length, lwip_ntohl(state.lan.ip.addr),
                                          lwip_ntohl(state.lan.netmask.addr), NULL, 0);
    if (!size) return false;
    uint32_t client;
    memcpy(&client, in + 26, sizeof(client));
    if (client == state.lan.gw.addr) return false;
    if (!learn_client(in + 6, client)) return false;
    dns_job_t *job = malloc(sizeof(*job));
    if (job) {
        job->epoch = state.epoch;
        job->len = bridge_dns_frame_reply(in, length, lwip_ntohl(state.lan.ip.addr),
                                            lwip_ntohl(state.lan.netmask.addr), job->frame, sizeof(job->frame));
        if (!job->len || xQueueSend(s_dns_queue, &job, 0) != pdTRUE) free(job);
    }
    /* A valid query for our name is consumed even if resources are temporarily
     * exhausted. Forwarding it would race the local answer with host NXDOMAIN.
     * The input remains owned by bridgeif; this hook never retains it.
     */
    return true;
}

static void dns_frames(void *arg)
{
    dns_job_t *job;
    for (;;) {
        if (xQueueReceive(s_dns_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        router_network_state_t state = router_network_state();
        if (state.online && state.epoch == job->epoch &&
            esp_netif_transmit(s_ap, job->frame, job->len) == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            ++s_intercepted;
            portEXIT_CRITICAL(&s_lock);
        }
        free(job);
    }
}

/* This optional endpoint lets the sharing computer query the name directly at
 * the bridge IP. Other names are not proxied here; PSP's normal DNS path remains
 * the transparent bridge, where only the exact local name is intercepted.
 */
static void dns_local(void *arg)
{
    uint8_t query[513], reply[512];
    int fd = -1;
    for (;;) {
        if (fd < 0) {
            fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53),
                                         .sin_addr.s_addr = INADDR_ANY};
            if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
                fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
                if (fd >= 0) close(fd);
                fd = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }
        fd_set set;
        FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval timeout = {.tv_usec = 250000};
        int ready = select(fd + 1, &set, NULL, NULL, &timeout);
        if (ready < 0) { close(fd); fd = -1; continue; }
        if (!ready) continue;
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int n = recvfrom(fd, query, sizeof(query), 0, (struct sockaddr *)&peer, &peer_size);
        router_network_state_t state = router_network_state();
        if (n < 0 || !state.online || !state.lan.netmask.addr ||
            !router_client_allowed(peer.sin_addr.s_addr, state.lan.ip.addr,
                                   state.lan.netmask.addr, state.lan.gw.addr)) continue;
        size_t size = bridge_dns_message_reply(query, n, lwip_ntohl(state.lan.ip.addr), reply, sizeof(reply));
        if (size && sendto(fd, reply, size, 0, (struct sockaddr *)&peer, peer_size) == (int)size) {
            portENTER_CRITICAL(&s_lock); ++s_direct; portEXIT_CRITICAL(&s_lock);
        }
    }
}

typedef struct { ip4_addr_t ip; struct eth_addr mac; } neighbor_t;
static neighbor_t s_owned[ROUTER_MAX_WIFI_CLIENTS];
static unsigned s_owned_count;
typedef struct { client_t clients[ROUTER_MAX_WIFI_CLIENTS]; bool online; } neighbors_context_t;

static bool same_neighbor(const neighbor_t *a, const neighbor_t *b)
{
    return ip4_addr_eq(&a->ip, &b->ip) && !memcmp(a->mac.addr, b->mac.addr, 6);
}

static esp_err_t bridge_sync_neighbors(void *arg)
{
    neighbors_context_t *context = arg;
    struct netif *netif = esp_netif_get_netif_impl(s_bridge);
    if (!netif) return ESP_ERR_INVALID_STATE;
    neighbor_t wanted[ROUTER_MAX_WIFI_CLIENTS];
    unsigned count = 0;
    for (unsigned i = 0; i < ROUTER_MAX_WIFI_CLIENTS && context->online; ++i) {
        client_t *c = &context->clients[i];
        ip4_addr_t ip = {.addr = c->ip};
        if (!c->active || !c->ip || !netif_is_up(netif) || ip4_addr_eq(&ip, netif_ip4_addr(netif)) ||
            ip4_addr_eq(&ip, netif_ip4_gw(netif)) ||
            !ip4_addr_netcmp(&ip, netif_ip4_addr(netif), netif_ip4_netmask(netif)) ||
            ip4_addr_isbroadcast(&ip, netif) || ip4_route(&ip) != netif) continue;
        wanted[count].ip = ip;
        memcpy(wanted[count++].mac.addr, c->mac, 6);
    }
    for (unsigned i = 0; i < s_owned_count; ++i) {
        bool retained = false;
        for (unsigned j = 0; j < count; ++j) retained |= same_neighbor(&s_owned[i], &wanted[j]);
        if (retained) continue;
        for (size_t j = 0; j < ARP_TABLE_SIZE; ++j) {
            ip4_addr_t *ip; struct netif *owner; struct eth_addr *mac;
            if (etharp_get_entry(j, &ip, &owner, &mac) && owner == netif &&
                ip4_addr_eq(ip, &s_owned[i].ip) && !memcmp(mac->addr, s_owned[i].mac.addr, 6)) {
                etharp_remove_static_entry(ip);
                break;
            }
        }
    }
    s_owned_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (etharp_add_static_entry(&wanted[i].ip, &wanted[i].mac) == ERR_OK) s_owned[s_owned_count++] = wanted[i];
    }
    return ESP_OK;
}

static void refresh_clients(bool online)
{
    wifi_sta_list_t stations = {0};
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return;
    client_t next[ROUTER_MAX_WIFI_CLIENTS] = {0};
    unsigned count = stations.num > ROUTER_MAX_WIFI_CLIENTS ? ROUTER_MAX_WIFI_CLIENTS : stations.num;
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = 0; j < ROUTER_MAX_WIFI_CLIENTS; ++j) {
            if (s_clients[j].active && !memcmp(s_clients[j].mac, stations.sta[i].mac, 6)) next[i] = s_clients[j];
        }
        memcpy(next[i].mac, stations.sta[i].mac, 6);
        next[i].active = true;
        next[i].rssi = stations.sta[i].rssi;
    }
    memcpy(s_clients, next, sizeof(next));
    portEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; i < count && online; ++i) {
        if (!next[i].ip) {
            esp_netif_pair_mac_ip_t pair = {0};
            memcpy(pair.mac, next[i].mac, 6);
            if (esp_netif_arp_get_client_by_mac(s_bridge, &pair) == ESP_OK && pair.ip.addr) {
                next[i].ip = pair.ip.addr;
                learn_client(pair.mac, pair.ip.addr);
            }
        }
    }
    neighbors_context_t context = {.online = online};
    memcpy(context.clients, next, sizeof(next));
    (void)esp_netif_tcpip_exec(bridge_sync_neighbors, &context);
}

router_lan_status_t router_network_lan_status(void)
{
    router_lan_status_t result = {.external_dhcp = true, .clients_valid = true,
                                  .dhcp_valid = true, .arp_valid = true};
    result.up = s_bridge && esp_netif_is_netif_up(s_bridge);
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < ROUTER_MAX_WIFI_CLIENTS; ++i) {
        if (!s_clients[i].active) continue;
        router_wifi_client_t *client = &result.clients[result.count++];
        memcpy(client->mac, s_clients[i].mac, 6);
        client->dhcp_ip.addr = s_clients[i].ip; /* Observed client IP, no local DHCP server. */
        client->rssi = s_clients[i].rssi;
    }
    result.name_intercepted = s_intercepted;
    result.name_direct = s_direct;
    portEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; i < result.count; ++i) {
        esp_netif_pair_mac_ip_t pair = {0};
        memcpy(pair.mac, result.clients[i].mac, 6);
        if (esp_netif_arp_get_client_by_mac(s_bridge, &pair) == ESP_OK) result.clients[i].arp_ip = pair.ip;
        else result.arp_valid = false;
    }
    return result;
}

void bridge_proxy_init(esp_netif_t *bridge, esp_netif_t *ap)
{
    s_bridge = bridge; s_ap = ap;
    s_dns_queue = xQueueCreate(8, sizeof(dns_job_t *));
    ESP_ERROR_CHECK(s_dns_queue ? ESP_OK : ESP_ERR_NO_MEM);
    esp_sntp_config_t ntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                       ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    ntp.start = false;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
    ESP_ERROR_CHECK(xTaskCreate(dns_frames, "bridge_dns_tx", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(dns_local, "bridge_name", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void bridge_proxy_poll(bool link_up)
{
    router_network_state_t next = router_network_state();
    esp_netif_ip_info_t ip = {0}; esp_netif_dns_info_t dns = {0};
    if (link_up) { esp_netif_get_ip_info(s_bridge, &ip); esp_netif_get_dns_info(s_bridge, ESP_NETIF_DNS_MAIN, &dns); }
    esp_ip4_addr_t resolver = {0};
    if (dns.ip.type == ESP_IPADDR_TYPE_V4) resolver = dns.ip.u_addr.ip4;
    bool online = link_up && ip.ip.addr && ip.netmask.addr && ip.gw.addr;
    bool changed = memcmp(&ip, &next.wan, sizeof(ip)) || next.dns.addr != resolver.addr || next.online != online;
    if (changed) {
        ++next.epoch;
        next.wan = ip;
        if (ip.ip.addr) next.lan = ip;
        next.dns = resolver;
        next.online = online;
        portENTER_CRITICAL(&s_lock);
        s_state = next;
        for (unsigned i = 0; i < ROUTER_MAX_WIFI_CLIENTS; ++i) s_clients[i].ip = 0;
        portEXIT_CRITICAL(&s_lock);
        if (online) {
            ESP_ERROR_CHECK(esp_netif_set_default_netif(s_bridge));
            ESP_ERROR_CHECK(esp_netif_sntp_start());
        }
    }
    static TickType_t previous;
    TickType_t now = xTaskGetTickCount();
    if (changed || now - previous >= pdMS_TO_TICKS(250)) {
        previous = now;
        refresh_clients(online);
    }
}
#endif
