/* Descriptor layout follows TinyUSB's net_lwip_webserver example (MIT). */
#include <stdio.h>
#include "sdkconfig.h"
#include "usb_descriptors.h"

extern uint8_t tud_network_mac_address[6];

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_NET, STR_MAC };
enum { ITF_NET, ITF_NET_DATA, ITF_COUNT };

#if CONFIG_PSP_USB_RNDIS
#define NET_DESC_LEN TUD_RNDIS_DESC_LEN
#define PRODUCT_ID 0x4011
#define PRODUCT_NAME "PSP Router RNDIS"
#else
#define NET_DESC_LEN TUD_CDC_ECM_DESC_LEN
#define PRODUCT_ID 0x4012
#define PRODUCT_NAME "PSP Router ECM"
#endif

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0201,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303a, /* Espressif example VID; allocate a VID/PID before distribution. */
    .idProduct = PRODUCT_ID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0,
                          TUD_CONFIG_DESC_LEN + NET_DESC_LEN, 0, 500),
#if CONFIG_PSP_USB_RNDIS
    TUD_RNDIS_DESCRIPTOR(ITF_NET, STR_NET, 0x81, 8, 0x02, 0x82, 64),
#else
    TUD_CDC_ECM_DESCRIPTOR(ITF_NET, STR_NET, STR_MAC, 0x81, 64, 0x02, 0x82, 64, 1514),
#endif
};

static char serial[13];
static char host_mac_string[13];
static const char *strings[] = {
    (const char[]){0x09, 0x04}, "ESP32 PSP Router", PRODUCT_NAME,
    serial, "USB bridge port", host_mac_string,
};

/* BOS + Microsoft OS 2.0 binds only interface 0 to Windows' inbox RNDIS driver.
 * Unlike MS OS 1.0 this needs no special string at index 0xEE (the Espressif
 * descriptor helper supports only eight strings). Windows 10/11 are targeted.
 */
static const uint8_t bos_descriptor[] = {
#if CONFIG_PSP_USB_RNDIS
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(46, 0x20),
#else
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN, 0),
#endif
};

#if CONFIG_PSP_USB_RNDIS
static const uint8_t microsoft_os_20[] = {
    10, 0, 0, 0, U32_TO_U8S_LE(0x06030000), 46, 0,
    8, 0, 1, 0, 0, 0, 36, 0, /* Configuration index 0. */
    8, 0, 2, 0, ITF_NET, 0, 28, 0,
    20, 0, 3, 0, 'R', 'N', 'D', 'I', 'S', 0, 0, 0,
    '5', '1', '6', '2', '0', '0', '1', 0,
};
_Static_assert(sizeof(microsoft_os_20) == 46, "MS OS descriptor length");
#endif
_Static_assert(sizeof(configuration_descriptor) ==
               TUD_CONFIG_DESC_LEN + NET_DESC_LEN, "Config length");

const uint8_t *tud_descriptor_bos_cb(void)
{
    return bos_descriptor;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                const tusb_control_request_t *request)
{
#if CONFIG_PSP_USB_RNDIS
    if (request->bmRequestType != 0xc0 || request->bRequest != 0x20 ||
        request->wIndex != 7 || request->wValue != 0) {
        return false;
    }
    return stage != CONTROL_STAGE_SETUP ||
           tud_control_xfer(rhport, request, (void *)microsoft_os_20, sizeof(microsoft_os_20));
#else
    return false;
#endif
}

void usb_descriptors_configure(tinyusb_config_t *config, const uint8_t device_mac[6])
{
    uint8_t host_mac[6];
    for (int i = 0; i < 6; ++i) {
        host_mac[i] = tud_network_mac_address[i];
    }
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             device_mac[0], device_mac[1], device_mac[2], device_mac[3], device_mac[4], device_mac[5]);
    snprintf(host_mac_string, sizeof(host_mac_string), "%02X%02X%02X%02X%02X%02X",
             host_mac[0], host_mac[1], host_mac[2], host_mac[3], host_mac[4], host_mac[5]);
    config->descriptor.device = &device_descriptor;
    config->descriptor.full_speed_config = configuration_descriptor;
    config->descriptor.string = strings;
    config->descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
}
