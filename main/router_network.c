#include <string.h>
#include "sdkconfig.h"
#if CONFIG_PSP_MODE_ROUTER
#include "freertos/FreeRTOS.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/def.h"
#include "dhcpserver/dhcpserver.h"
#include "router_network.h"
#include "router_policy.h"

static esp_netif_t *s_wan, *s_lan;
static router_network_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_napt;

router_network_state_t router_network_state(void)
{
    portENTER_CRITICAL(&s_lock);
    router_network_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
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
