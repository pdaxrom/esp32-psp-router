idf_component_register(SRCS "main.c" "usb_net.c" "usb_descriptors.c" "usb_ecm_guard.c" "bridge_policy.c"
                            "router_policy.c" "router_network.c" "router_dns.c"
                            "proxy_protocol.c" "browser_proxy.c" "bridge_dns.c" "bridge_proxy.c"
                            "status_rgb.c" "status_probe.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_psram esp_timer spi_flash
                                nvs_flash esp_tinyusb lwip wpa_supplicant esp_http_client mbedtls esp_driver_rmt)

# Only legacy WPA1 needs EAPOL v1. WPA2 uses the unmodified IDF authenticator.
if(CONFIG_PSP_AP_WPA1)
    # Keep the compatibility wrapper's struct layout identical to the authenticator.
    idf_component_get_property(psp_wpa_lib wpa_supplicant COMPONENT_LIB)
    target_sources(${psp_wpa_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/wpa_compat.c")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=wpa_init")
endif()
