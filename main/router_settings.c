#include <string.h>
#include "sdkconfig.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "bridge_policy.h"
#include "router_settings.h"

static esp_netif_t *s_ap;

static esp_err_t start_radio(esp_netif_t *ap)
{
    esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) return result;

    result = esp_wifi_register_if_rxcb(esp_netif_get_io_driver(ap), esp_netif_receive, ap);
    if (result == ESP_OK) {
        result = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    }
    if (result != ESP_OK) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    return result;
}

esp_err_t router_wifi_ap_start(esp_netif_t *ap)
{
    uint8_t mac[6];
    esp_err_t result = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (result == ESP_OK) result = esp_netif_set_mac(ap, mac);
    if (result == ESP_OK) result = start_radio(ap);
    if (result != ESP_OK) return result;

    /* Keep the netif registered across radio restarts. lwIP installs the
     * bridge receive callback on this port, and router mode owns DHCP/NAPT here. */
    esp_netif_action_start(ap, NULL, 0, NULL);
    s_ap = ap;
    return esp_register_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
}

static bool wifi_settings_valid(const router_wifi_settings_t *settings)
{
    return memchr(settings->ssid, 0, sizeof(settings->ssid)) &&
           bridge_ssid_valid(settings->ssid) &&
           memchr(settings->password, 0, sizeof(settings->password)) &&
           bridge_password_valid(settings->password);
}

void router_wifi_settings_defaults(router_wifi_settings_t *settings)
{
    *settings = (router_wifi_settings_t){
        .ssid = "PSP-NET",
#ifdef CONFIG_PSP_AP_PASSWORD
        .password = CONFIG_PSP_AP_PASSWORD,
#else
        .password = "psp3000wifi",
#endif
    };
}

void router_wifi_settings_load(router_wifi_settings_t *settings)
{
    router_wifi_settings_defaults(settings);
    nvs_handle_t nvs;
    if (nvs_open("psp_config", NVS_READONLY, &nvs) != ESP_OK) return;
    router_wifi_settings_t stored;
    size_t length = sizeof(stored);
    if (nvs_get_blob(nvs, "wifi_v1", &stored, &length) == ESP_OK &&
        length == sizeof(stored) && wifi_settings_valid(&stored)) *settings = stored;
    nvs_close(nvs);
}

static esp_err_t save(const router_wifi_settings_t *settings)
{
    if (!wifi_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("psp_config", NVS_READWRITE, &nvs);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(nvs, "wifi_v1", settings, sizeof(*settings));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

bool router_wifi_settings_get_active(router_wifi_settings_t *settings)
{
    wifi_config_t active;
    if (esp_wifi_get_config(WIFI_IF_AP, &active) != ESP_OK) return false;
    bool valid = active.ap.ssid_len > 0 && active.ap.ssid_len <= sizeof(active.ap.ssid);
    if (valid) {
        memcpy(settings->ssid, active.ap.ssid, active.ap.ssid_len);
        settings->ssid[active.ap.ssid_len] = '\0';
#if !CONFIG_PSP_AP_OPEN
        memcpy(settings->password, active.ap.password, sizeof(settings->password) - 1);
        settings->password[sizeof(settings->password) - 1] = '\0';
#endif
    }
    mbedtls_platform_zeroize(&active, sizeof(active));
    return valid;
}

static esp_err_t configure_radio(wifi_config_t *config)
{
    esp_err_t result = esp_wifi_set_config(WIFI_IF_AP, config);
    if (result == ESP_OK) result = esp_wifi_disable_pmf_config(WIFI_IF_AP);
    return result;
}

static esp_err_t apply_radio(wifi_config_t *next)
{
    esp_err_t result = configure_radio(next);
    if (result != ESP_OK) return result;

    wifi_config_t active;
    result = esp_wifi_get_config(WIFI_IF_AP, &active);
    if (result == ESP_OK && (active.ap.ssid_len != next->ap.ssid_len ||
        memcmp(active.ap.ssid, next->ap.ssid, next->ap.ssid_len) ||
        strncmp((const char *)active.ap.password, (const char *)next->ap.password, sizeof(next->ap.password)) ||
        active.ap.authmode != next->ap.authmode || active.ap.channel != next->ap.channel ||
        active.ap.pairwise_cipher != next->ap.pairwise_cipher || active.ap.ssid_hidden != next->ap.ssid_hidden ||
        active.ap.pmf_cfg.required || active.ap.pmf_cfg.capable)) result = ESP_ERR_INVALID_STATE;
    mbedtls_platform_zeroize(&active, sizeof(active));
    return result == ESP_OK ? start_radio(s_ap) : result;
}

esp_err_t router_wifi_settings_apply(const router_wifi_settings_t *settings)
{
    if (!s_ap) return ESP_ERR_INVALID_STATE;
    if (!wifi_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    wifi_config_t previous;
    esp_err_t result = esp_wifi_get_config(WIFI_IF_AP, &previous);
    if (result != ESP_OK) return result;

    wifi_config_t next = previous;
    memset(next.ap.ssid, 0, sizeof(next.ap.ssid));
    next.ap.ssid_len = strlen(settings->ssid);
    memcpy(next.ap.ssid, settings->ssid, next.ap.ssid_len);
#if !CONFIG_PSP_AP_OPEN
    memset(next.ap.password, 0, sizeof(next.ap.password));
    memcpy(next.ap.password, settings->password, strlen(settings->password));
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deauth_sta(0));
    result = esp_wifi_stop();
    if (result == ESP_OK) {
        result = apply_radio(&next);
        if (result == ESP_OK) {
            result = save(settings);
            if (result != ESP_OK) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
        }
        if (result != ESP_OK) ESP_ERROR_CHECK_WITHOUT_ABORT(apply_radio(&previous));
    }
    mbedtls_platform_zeroize(&next, sizeof(next));
    mbedtls_platform_zeroize(&previous, sizeof(previous));
    return result;
}
