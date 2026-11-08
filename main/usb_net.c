#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_netif_defaults.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device/dcd.h"
#include "usb_net.h"
#include "usb_descriptors.h"
#include "usb_ecm_guard.h"
#include "bridge_policy.h"
#include "bridge_proxy.h"

/* The netif TX callback runs in the TCP/IP task. It MUST NOT wait for the
 * TinyUSB task: that task also feeds the TCP/IP mailbox on reception.
 * This bounded queue owns a copy; only the TinyUSB task touches USB endpoints.
 */
#define TX_QUEUE_LEN 12
typedef struct {
    uint32_t epoch;
    uint32_t queued_ms;
    uint16_t len;
    uint8_t data[];
} tx_packet_t;

static esp_netif_t *s_port;
static QueueHandle_t s_tx_queue;
static atomic_uint s_mounted, s_suspended, s_accept_rx;
static atomic_uint s_epoch;
static uint8_t s_bridge_mac[6];
uint8_t tud_network_mac_address[6]; /* MAC of the host's USB NIC, NOT the bridge. */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t transmit(void *handle, void *buffer, size_t len)
{
    uint32_t epoch = atomic_load(&s_epoch);
    if (!atomic_load(&s_mounted) || atomic_load(&s_suspended) || len < 14 || len > 1514) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_PSP_BRIDGE_PROXY
    if (bridge_proxy_intercept(buffer, len)) return ESP_OK;
#endif
    tx_packet_t *packet = malloc(sizeof(*packet) + len);
    if (!packet) {
        return ESP_ERR_NO_MEM;
    }
    packet->epoch = epoch;
    packet->queued_ms = now_ms();
    packet->len = len;
    memcpy(packet->data, buffer, len);
    if (epoch != atomic_load(&s_epoch) || !atomic_load(&s_mounted) || atomic_load(&s_suspended) ||
        xQueueSend(s_tx_queue, &packet, 0) != pdTRUE) {
        free(packet);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void free_rx(void *handle, void *buffer)
{
    free(buffer);
}

/* Called by TinyUSB's task, not from the USB ISR. At most one frame per SOF
 * (1 ms) plus bounded stale-frame cleanup. No defer-event flooding or busy wait.
 */
void tud_sof_cb(uint32_t frame_count)
{
    tx_packet_t *packet;
    for (int i = 0; i < TX_QUEUE_LEN; ++i) {
        if (xQueuePeek(s_tx_queue, &packet, 0) != pdTRUE) {
            return;
        }
        bool current = bridge_frame_current(packet->epoch, atomic_load(&s_epoch),
                                             now_ms() - packet->queued_ms, packet->len);
        if (!current || !tud_ready()) {
            xQueueReceive(s_tx_queue, &packet, 0);
            free(packet);
            continue;
        }
        if (!usb_ecm_data_ready() || !tud_network_can_xmit(packet->len)) {
            return;
        }
        xQueueReceive(s_tx_queue, &packet, 0);
        /* tud_network_xmit_cb copies and frees the packet synchronously. */
        tud_network_xmit(packet, packet->len);
        return;
    }
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    tx_packet_t *packet = ref;
    uint16_t len = packet->len;
    memcpy(dst, packet->data, len);
    free(packet);
    return len;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (atomic_load(&s_accept_rx) && size >= 14 && size <= 1514) {
        void *copy = malloc(size);
        if (copy) {
            memcpy(copy, src, size);
            /* esp-netif takes ownership, including on input errors. */
            esp_netif_receive(s_port, copy, size, NULL);
        }
    }
    tud_network_recv_renew();
    return true;
}

void tud_network_init_cb(void)
{
}

/* Reset is significant even when a host resets USB without an unplug event.
 * All atomics reside in internal DRAM. ESP-IDF's PSRAM atomic workaround
 * dispatches these 32-bit operations to S32C1I; its fallback is ISR-safe too.
 */
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
    if (eventid == DCD_EVENT_BUS_RESET || eventid == DCD_EVENT_UNPLUGGED) {
        atomic_store(&s_accept_rx, false);
        atomic_store(&s_mounted, false);
        atomic_fetch_add(&s_epoch, 1);
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    atomic_store(&s_suspended, true);
    atomic_store(&s_accept_rx, false);
    atomic_fetch_add(&s_epoch, 1);
}

void tud_resume_cb(void)
{
    atomic_store(&s_suspended, false);
}

static void device_event(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        atomic_store(&s_suspended, false);
        atomic_store(&s_mounted, true);
        tud_sof_cb_enable(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        atomic_store(&s_accept_rx, false);
        atomic_store(&s_mounted, false);
        atomic_fetch_add(&s_epoch, 1); /* Also covers SET_CONFIGURATION(0), without a bus reset. */
    }
}

esp_netif_t *usb_net_create(const uint8_t bridge_mac[6])
{
    memcpy(s_bridge_mac, bridge_mac, 6);
    ESP_ERROR_CHECK(esp_derive_local_mac(tud_network_mac_address, bridge_mac));
    tud_network_mac_address[5] ^= 0x80;
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_packet_t *));
    ESP_ERROR_CHECK(s_tx_queue ? ESP_OK : ESP_ERR_NO_MEM);
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
#if CONFIG_PSP_MODE_BRIDGE
    base.flags = 0;
    base.ip_info = NULL; /* Bridge ports carry no IP. */
#else
    base.route_prio = 100; /* USB WAN DHCP client, preferred over the AP. */
#endif
    base.if_key = "USB_PORT";
    base.if_desc = "usb";
    memcpy(base.mac, bridge_mac, 6);
#if CONFIG_PSP_MODE_ROUTER
    ESP_ERROR_CHECK(esp_derive_local_mac(base.mac, bridge_mac));
    base.mac[5] ^= 0x40; /* Distinct from AP and host NIC; host MAC stays stable. */
#endif
    const esp_netif_driver_ifconfig_t driver = {
        .handle = &s_port, /* esp-netif requires a non-NULL IO driver handle at start. */
        .transmit = transmit,
        .driver_free_rx_buffer = free_rx,
    };
    const esp_netif_config_t config = {
        .base = &base,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_port = esp_netif_new(&config);
    ESP_ERROR_CHECK(s_port ? ESP_OK : ESP_ERR_NO_MEM);
    return s_port;
}

void usb_net_start(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(device_event);
    usb_descriptors_configure(&config, s_bridge_mac);
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
}

void usb_net_accept_rx(bool accept)
{
    atomic_store(&s_accept_rx, accept);
}

usb_net_status_t usb_net_status(void)
{
    return (usb_net_status_t) {
        .active = atomic_load(&s_mounted) && !atomic_load(&s_suspended),
        .epoch = atomic_load(&s_epoch),
    };
}
