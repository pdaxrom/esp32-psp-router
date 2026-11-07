idf_component_register(SRCS "main.c" "usb_net.c" "usb_descriptors.c" "usb_ecm_guard.c" "bridge_policy.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_psram esp_timer spi_flash
                                nvs_flash esp_tinyusb lwip wpa_supplicant)

# Keep the compatibility wrapper's struct layout identical to the authenticator.
idf_component_get_property(psp_wpa_lib wpa_supplicant COMPONENT_LIB)
target_sources(${psp_wpa_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/wpa_compat.c")
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=wpa_init")
