#include "sdkconfig.h"
#include "status_rgb.h"
#if CONFIG_PSP_STATUS_RGB
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "status_probe.h"

#define PROBE_DNS_MS 3000
#define PROBE_HTTP_MS 3000
#define PROBE_INTERVAL_MS 15000
#define CLIENTS 4
static esp_netif_t *s_netif;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static status_rgb_state_t s_state;
static esp_netif_ip_info_t s_ip;
static uint32_t s_dns, s_usb_epoch;
static uint8_t s_clients[CLIENTS][6];
static TaskHandle_t s_probe_task;
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
/* Static storage also remains valid if a TX completion times out. One complete
 * frame fits in RMT SRAM: no refill ISR/bit-banging competing with USB or Wi-Fi.
 */
static rmt_symbol_word_t s_symbols[25];

status_rgb_state_t status_rgb_state(void)
{
    portENTER_CRITICAL(&s_lock);
    status_rgb_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

void status_rgb_station(const uint8_t mac[6], bool connected)
{
    const uint8_t empty[6] = {0};
    portENTER_CRITICAL(&s_lock);
    int slot = -1;
    for (unsigned i = 0; i < CLIENTS; ++i) {
        if (!memcmp(s_clients[i], mac, 6)) { slot = i; break; }
        if (!memcmp(s_clients[i], empty, 6)) slot = i;
    }
    if (slot >= 0) memcpy(s_clients[slot], connected ? mac : empty, 6);
    s_state.clients = 0;
    for (unsigned i = 0; i < CLIENTS; ++i) s_state.clients += memcmp(s_clients[i], empty, 6) != 0;
    portEXIT_CRITICAL(&s_lock);
}

void status_rgb_poll(bool usb, uint32_t usb_epoch)
{
    if (!s_netif) return;
    esp_netif_ip_info_t ip = {0};
    esp_netif_dns_info_t dns = {0};
    if (usb) {
        esp_netif_get_ip_info(s_netif, &ip);
        esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    uint32_t resolver = dns.ip.type == ESP_IPADDR_TYPE_V4 ? dns.ip.u_addr.ip4.addr : 0;
    bool configured = usb && ip.ip.addr && ip.netmask.addr && ip.gw.addr && resolver;
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state.usb != usb || s_usb_epoch != usb_epoch ||
                   memcmp(&ip, &s_ip, sizeof(ip)) || s_dns != resolver;
    if (changed) {
        ++s_state.generation;
        s_state.usb = usb;
        s_state.configured = configured;
        s_state.internet = false;
        s_ip = ip; s_dns = resolver; s_usb_epoch = usb_epoch;
    }
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_probe_task) xTaskNotifyGive(s_probe_task);
}

static bool current(uint32_t generation)
{
    status_rgb_state_t state = status_rgb_state();
    return state.configured && state.generation == generation;
}

/* All resolver contexts have process lifetime. A delayed lwIP callback after
 * our 3-second wait cannot access a deleted semaphore or a worker stack frame.
 * Only the TCP/IP task touches pending/generation in these contexts.
 */
typedef struct { uint32_t generation, address; } dns_result_t;
typedef struct {
    const char *host;
    QueueHandle_t result;
    uint32_t generation;
    bool pending;
} resolver_t;
static resolver_t s_resolvers[] = {
    {.host = "connectivitycheck.gstatic.com"},
    {.host = "cp.cloudflare.com"},
};
typedef struct { resolver_t *resolver; uint32_t generation; } dns_start_t;

static void dns_found(const char *name, const ip_addr_t *address, void *arg)
{
    resolver_t *resolver = arg;
    dns_result_t result = {.generation = resolver->generation};
    if (address && IP_IS_V4(address)) result.address = ip_2_ip4(address)->addr;
    resolver->pending = false;
    xQueueOverwrite(resolver->result, &result);
}

static esp_err_t start_dns(void *arg)
{
    dns_start_t *start = arg;
    resolver_t *resolver = start->resolver;
    if (resolver->pending) return ESP_ERR_INVALID_STATE;
    xQueueReset(resolver->result);
    resolver->generation = start->generation;
    resolver->pending = true;
    ip_addr_t address;
    err_t error = dns_gethostbyname_addrtype(resolver->host, &address, dns_found,
                                            resolver, LWIP_DNS_ADDRTYPE_IPV4);
    if (error != ERR_INPROGRESS) dns_found(resolver->host, error == ERR_OK ? &address : NULL, resolver);
    return ESP_OK;
}

static bool wait_socket(int fd, bool writing, int64_t deadline, uint32_t generation)
{
    while (current(generation) && esp_timer_get_time() < deadline) {
        fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval wait = {.tv_usec = 100000};
        int n = select(fd + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &wait);
        if (n > 0) return true;
        if (n < 0 && errno != EINTR) return false;
    }
    return false;
}

static bool probe(resolver_t *resolver, uint32_t generation)
{
    dns_start_t start = {.resolver = resolver, .generation = generation};
    if (esp_netif_tcpip_exec(start_dns, &start) != ESP_OK) return false;
    dns_result_t result;
    if (xQueueReceive(resolver->result, &result, pdMS_TO_TICKS(PROBE_DNS_MS)) != pdTRUE ||
        !result.address || result.generation != generation || !current(generation)) return false;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    bool success = false;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;
    int64_t deadline = esp_timer_get_time() + PROBE_HTTP_MS * 1000;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(80),
                                 .sin_addr.s_addr = result.address};
    int connected = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (connected && errno != EINPROGRESS) goto done;
    if (connected && !wait_socket(fd, true, deadline, generation)) goto done;
    int error = 0; socklen_t error_size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) || error) goto done;
    char data[512];
    int length = snprintf(data, sizeof(data), "GET /generate_204 HTTP/1.0\r\nHost: %s\r\n"
                          "Connection: close\r\nUser-Agent: PSP-Router-status\r\n\r\n", resolver->host);
    for (int sent = 0; sent < length;) {
        if (!wait_socket(fd, true, deadline, generation)) goto done;
        int n = send(fd, data + sent, length - sent, 0);
        if (n > 0) sent += n;
        else if (n == 0 || (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)) goto done;
    }
    size_t received = 0;
    while (received < sizeof(data)) {
        if (!wait_socket(fd, false, deadline, generation)) goto done;
        int n = recv(fd, data + received, sizeof(data) - received, 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) continue;
            break;
        }
        received += n;
        if (status_probe_response(data, received)) { success = true; break; }
    }
done:
    close(fd);
    return success && current(generation);
}

static void probe_task(void *arg)
{
    for (;;) {
        status_rgb_state_t state = status_rgb_state();
        bool available = false;
        for (unsigned i = 0; state.configured && i < sizeof(s_resolvers)/sizeof(*s_resolvers) && current(state.generation); ++i) {
            if (probe(&s_resolvers[i], state.generation)) { available = true; break; }
        }
        portENTER_CRITICAL(&s_lock);
        if (s_state.generation == state.generation) s_state.internet = available;
        portEXIT_CRITICAL(&s_lock);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
}

static void led_task(void *arg)
{
    uint32_t previous = UINT32_MAX;
    for (;;) {
        status_rgb_state_t state = status_rgb_state();
        uint8_t r = 0, g = 0, b = 0;
        if (!state.usb) r = CONFIG_PSP_STATUS_RGB_BRIGHTNESS;
        else if (!state.internet) { r = CONFIG_PSP_STATUS_RGB_BRIGHTNESS; g = r; }
        else g = CONFIG_PSP_STATUS_RGB_BRIGHTNESS;
        /* A 200-ms cyan pulse every 2 seconds overlays the USB/internet colour. */
        if (state.clients && (esp_timer_get_time() / 1000) % 2000 < 200) {
            r = 0; g = b = CONFIG_PSP_STATUS_RGB_BRIGHTNESS;
        }
        uint32_t color = (uint32_t)g << 16 | (uint32_t)r << 8 | b; /* WS2812: GRB, MSB first. */
        if (color != previous) {
            for (unsigned i = 0; i < 24; ++i) {
                bool one = color & (1u << (23 - i));
                s_symbols[i] = (rmt_symbol_word_t){.level0 = 1, .duration0 = one ? 9 : 3,
                                                 .level1 = 0, .duration1 = one ? 3 : 9};
            }
            s_symbols[24] = (rmt_symbol_word_t){.duration0 = 1500, .duration1 = 1500};
            rmt_transmit_config_t tx = {.flags.queue_nonblocking = true};
            if (rmt_transmit(s_channel, s_encoder, s_symbols, sizeof(s_symbols), &tx) != ESP_OK ||
                rmt_tx_wait_all_done(s_channel, 20) != ESP_OK) {
                /* LED failure must not reboot or hang the router, nor modify a
                 * buffer still owned by RMT. Stop only this optional task.
                 */
                portENTER_CRITICAL(&s_lock); s_state.enabled = false; portEXIT_CRITICAL(&s_lock);
                vTaskDelete(NULL);
            }
            previous = color;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void status_rgb_init(esp_netif_t *internet_netif)
{
    rmt_tx_channel_config_t config = {.gpio_num = 48, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, .mem_block_symbols = 64, .trans_queue_depth = 1};
    rmt_copy_encoder_config_t encoder = {};
    bool enabled = false;
    if (rmt_new_tx_channel(&config, &s_channel) != ESP_OK) return;
    if (rmt_new_copy_encoder(&encoder, &s_encoder) != ESP_OK) goto error;
    if (rmt_enable(s_channel) != ESP_OK) goto error;
    enabled = true;
    for (unsigned i = 0; i < sizeof(s_resolvers)/sizeof(*s_resolvers); ++i) {
        s_resolvers[i].result = xQueueCreate(1, sizeof(dns_result_t));
        if (!s_resolvers[i].result) goto error;
    }
    portENTER_CRITICAL(&s_lock); s_state.enabled = true; portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(led_task, "status_rgb", 3072, NULL, 2, NULL) != pdPASS) goto error;
    s_netif = internet_netif;
    /* Probe allocation failure leaves the useful USB/client indication alive. */
    (void)xTaskCreate(probe_task, "status_probe", 4096, NULL, 2, &s_probe_task);
    return;
error:
    portENTER_CRITICAL(&s_lock); s_state.enabled = false; portEXIT_CRITICAL(&s_lock);
    for (unsigned i = 0; i < sizeof(s_resolvers)/sizeof(*s_resolvers); ++i) {
        if (s_resolvers[i].result) { vQueueDelete(s_resolvers[i].result); s_resolvers[i].result = NULL; }
    }
    if (enabled) rmt_disable(s_channel);
    if (s_encoder) rmt_del_encoder(s_encoder);
    rmt_del_channel(s_channel);
}
#else
status_rgb_state_t status_rgb_state(void) { return (status_rgb_state_t){0}; }
void status_rgb_init(esp_netif_t *netif) { (void)netif; }
void status_rgb_poll(bool usb, uint32_t epoch) { (void)usb; (void)epoch; }
void status_rgb_station(const uint8_t mac[6], bool connected) { (void)mac; (void)connected; }
#endif
