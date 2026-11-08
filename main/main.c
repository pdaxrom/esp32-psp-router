/* USB WAN / PSP Wi-Fi LAN, with the previous transparent bridge as an option. */
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"
#include "usb_net.h"
#include "bridge_policy.h"
#include "lwip/def.h"
#include "router_network.h"
#include "router_dns.h"
#include "browser_proxy.h"
#include "bridge_proxy.h"
#include "bridge_dns.h"

static const char *TAG = "psp_bridge";
static EventGroupHandle_t s_events;
static esp_netif_t *s_usb, *s_ap;
#if CONFIG_PSP_MODE_BRIDGE
static esp_netif_t *s_bridge;
#endif
#define AP_STARTED BIT0

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_START) {
            xEventGroupSetBits(s_events, AP_STARTED);
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            const wifi_event_ap_staconnected_t *e = data;
#if CONFIG_PSP_BRIDGE_PROXY
            bridge_proxy_station_event(e->mac, true);
#endif
            ESP_LOGI(TAG, "Wi-Fi client " MACSTR " joined (AID %d)", MAC2STR(e->mac), e->aid);
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            const wifi_event_ap_stadisconnected_t *e = data;
#if CONFIG_PSP_BRIDGE_PROXY
            bridge_proxy_station_event(e->mac, false);
#endif
            ESP_LOGI(TAG, "Wi-Fi client " MACSTR " left (reason %d)", MAC2STR(e->mac), e->reason);
        } else if (id == WIFI_EVENT_AP_WRONG_PASSWORD) {
            const wifi_event_ap_wrong_password_t *e = data;
            ESP_LOGW(TAG, "WPA MIC verification failed for " MACSTR, MAC2STR(e->mac));
        }
    }
#if CONFIG_PSP_MODE_BRIDGE
    else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        if (e->esp_netif == s_bridge) {
            static esp_netif_ip_info_t previous;
            if (previous.ip.addr &&
                ((previous.ip.addr & previous.netmask.addr) != (e->ip_info.ip.addr & e->ip_info.netmask.addr) ||
                 previous.netmask.addr != e->ip_info.netmask.addr || previous.gw.addr != e->ip_info.gw.addr)) {
                /* A host sharing-subnet change invalidates PSP's old DHCP lease. */
                ESP_LOGW(TAG, "Host subnet/gateway changed: reconnecting Wi-Fi clients for DHCP");
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deauth_sta(0));
            }
            previous = e->ip_info;
            ESP_LOGI(TAG, "Bridge management IP=" IPSTR " mask=" IPSTR " gateway=" IPSTR,
                     IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
        }
    }
#endif
}

static void init_wifi(void)
{
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t config = {
        .ap = {
            .ssid = "PSP-NET",
            .ssid_len = 7,
            .ssid_hidden = 0,
            .channel = 6,
            .max_connection = 4,
            .beacon_interval = 100,
            .dtim_period = 1,
            .pmf_cfg = {.required = false},
#if CONFIG_PSP_AP_OPEN
            .authmode = WIFI_AUTH_OPEN,
#elif CONFIG_PSP_AP_WPA2
            .password = CONFIG_PSP_AP_PASSWORD,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pairwise_cipher = WIFI_CIPHER_TYPE_CCMP,
#else
            .password = CONFIG_PSP_AP_PASSWORD,
            .authmode = WIFI_AUTH_WPA_PSK,
            .pairwise_cipher = WIFI_CIPHER_TYPE_TKIP,
#endif
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_disable_pmf_config(WIFI_IF_AP));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Read back settings. A driver error must never silently change security. */
    wifi_config_t actual;
    uint8_t protocol;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_AP, &actual));
    ESP_ERROR_CHECK(esp_wifi_get_protocol(WIFI_IF_AP, &protocol));
    ESP_LOGI(TAG, "AP readback: auth=%d cipher=%d protocol=0x%02x channel=%u",
             actual.ap.authmode, actual.ap.pairwise_cipher, protocol, actual.ap.channel);
    bool correct = protocol == WIFI_PROTOCOL_11B && actual.ap.channel == 6 && !actual.ap.ssid_hidden &&
                   !actual.ap.pmf_cfg.required && !actual.ap.pmf_cfg.capable;
#if CONFIG_PSP_AP_OPEN
    correct = correct && actual.ap.authmode == WIFI_AUTH_OPEN;
#elif CONFIG_PSP_AP_WPA2
    correct = correct && actual.ap.authmode == WIFI_AUTH_WPA2_PSK &&
              actual.ap.pairwise_cipher == WIFI_CIPHER_TYPE_CCMP;
#else
    correct = correct && actual.ap.authmode == WIFI_AUTH_WPA_PSK &&
              actual.ap.pairwise_cipher == WIFI_CIPHER_TYPE_TKIP;
#endif
    ESP_ERROR_CHECK(correct ? ESP_OK : ESP_ERR_INVALID_STATE);

    esp_netif_inherent_config_t ap = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
#if CONFIG_PSP_MODE_BRIDGE
    ap.flags = ESP_NETIF_FLAG_AUTOUP;
    ap.ip_info = NULL; /* No AP DHCP server, no 192.168.4.1 fallback. */
#else
    static const esp_netif_ip_info_t initial_lan = {
        .ip.addr = PP_HTONL(0xc0a83201), .gw.addr = PP_HTONL(0xc0a83201),
        .netmask.addr = PP_HTONL(0xffffff00),
    };
    ap.ip_info = &initial_lan;
#endif
    s_ap = esp_netif_create_wifi(WIFI_IF_AP, &ap);
    ESP_ERROR_CHECK(s_ap ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());
}

#if CONFIG_PSP_MODE_BRIDGE
static esp_err_t add_bridge_ports(void *ctx)
{
    /* Called in TCP/IP context before either port accepts user traffic. */
    esp_err_t ret = esp_netif_bridge_add_port(s_bridge, s_usb);
    return ret == ESP_OK ? esp_netif_bridge_add_port(s_bridge, s_ap) : ret;
}
#endif

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_events ? ESP_OK : ESP_ERR_NO_MEM);
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    ESP_LOGI(TAG, "Creating native USB Ethernet port");
    s_usb = usb_net_create(mac);
    ESP_LOGI(TAG, "Starting TinyUSB");
    usb_net_start();
    ESP_LOGI(TAG, "USB Ethernet initialized");

    uint32_t s_flash_size;
    ESP_ERROR_CHECK(esp_flash_get_physical_size(NULL, &s_flash_size));
    size_t s_psram_size = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
    bool memory_ok = s_flash_size == 4 * 1024 * 1024 && s_psram_size == 2 * 1024 * 1024;
#if CONFIG_PSP_AP_OPEN
    const bool password_ok = true;
#else
    const bool password_ok = bridge_password_valid(CONFIG_PSP_AP_PASSWORD);
#endif
    while (!memory_ok || !password_ok) {
        ESP_LOGE(TAG, "AP stopped: detected Flash=%" PRIu32 " PSRAM=%u; expected 4194304/2097152 bytes; passphrase %s",
                 s_flash_size, (unsigned)s_psram_size, password_ok ? "valid" : "INVALID (8..63 ASCII)");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGI(TAG, "Configuring PSP SoftAP");
    init_wifi();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
#if CONFIG_PSP_MODE_BRIDGE
    bridgeif_config_t bridge_info = {
        .max_fdb_dyn_entries = 16,
        .max_fdb_sta_entries = 2,
        .max_ports = 2,
    };
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_BR();
    base.bridge_info = &bridge_info;
    memcpy(base.mac, mac, 6);
    const esp_netif_config_t bridge_config = {
        .base = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_BR,
    };
    s_bridge = esp_netif_new(&bridge_config);
    ESP_ERROR_CHECK(s_bridge ? ESP_OK : ESP_ERR_NO_MEM);
#endif
    ESP_LOGI(TAG, "Starting bridge ports");
    esp_netif_action_start(s_usb, NULL, 0, NULL);
#if CONFIG_PSP_MODE_BRIDGE
    esp_netif_action_start(s_bridge, NULL, 0, NULL);
#if CONFIG_PSP_BRIDGE_PROXY
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_bridge, PSP_PROXY_HOSTNAME));
#endif
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_events, AP_STARTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
    ESP_ERROR_CHECK((bits & AP_STARTED) ? ESP_OK : ESP_ERR_TIMEOUT);
#if CONFIG_PSP_MODE_BRIDGE
    ESP_ERROR_CHECK(esp_netif_tcpip_exec(add_bridge_ports, NULL));
#if CONFIG_PSP_BRIDGE_PROXY
    bridge_proxy_init(s_bridge, s_ap);
    browser_proxy_start();
#endif
#else
    router_network_init(s_usb, s_ap);
    router_dns_start();
#if CONFIG_PSP_HTTP_PROXY
    browser_proxy_start();
#endif
#endif

    bool link_up = false;
    uint32_t previous_epoch = 0;
    for (;;) {
        usb_net_status_t usb = usb_net_status();
        if (link_up && (!usb.active || usb.epoch != previous_epoch)) {
            usb_net_accept_rx(false);
#if CONFIG_PSP_MODE_ROUTER
            router_network_poll(false);
#else
#if CONFIG_PSP_BRIDGE_PROXY
            bridge_proxy_poll(false);
#endif
            esp_netif_action_disconnected(s_bridge, NULL, 0, NULL);
#endif
            esp_netif_action_disconnected(s_usb, NULL, 0, NULL);
            link_up = false;
            ESP_LOGI(TAG, "USB link down/reset: bridge DHCP stopped; AP stays available");
        }
        if (!link_up && usb.active) {
            esp_netif_action_connected(s_usb, NULL, 0, NULL);
            usb_net_accept_rx(true);
#if CONFIG_PSP_MODE_BRIDGE
            esp_netif_action_connected(s_bridge, NULL, 0, NULL);
#endif
            link_up = true;
            ESP_LOGI(TAG, "USB link up: forwarding enabled, bridge management DHCP started");
        }
        previous_epoch = usb.epoch;
#if CONFIG_PSP_MODE_ROUTER
        router_network_poll(link_up);
#elif CONFIG_PSP_BRIDGE_PROXY
        bridge_proxy_poll(link_up);
#endif
        /* Never stop/recreate a bridge port on USB resets: lwIP's FDB retains
         * netif pointers and has no remove-port API. Only change link/up state.
         */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
