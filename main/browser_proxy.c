#include "sdkconfig.h"
#if CONFIG_PSP_HTTP_PROXY
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "browser_proxy.h"
#include "proxy_protocol.h"
#include "router_network.h"
#include "router_policy.h"
#include "bridge_dns.h"

#define PROXY_CLIENTS 2
#define BODY_LIMIT (4 * 1024 * 1024)
static SemaphoreHandle_t s_slots;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static struct { unsigned requests, connect_ms, total_ms, bytes; bool complete; } s_stats;
static struct { unsigned accepted, requests, errors; esp_ip4_addr_t last_ip; } s_lan_stats;
typedef struct { int fd; uint32_t epoch; bool lan; } connection_t;
typedef struct {
    char content_type[128], location[PROXY_URL_MAX], encoding[32];
    bool bad;
} response_meta_t;

static bool send_all(int fd, const void *data, size_t length, uint32_t epoch)
{
    const char *p = data;
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (length && esp_timer_get_time() < deadline) {
        if (epoch && router_network_state().epoch != epoch) return false;
        int n = send(fd, p, length, 0);
        if (n > 0) {
            p += n;
            length -= n;
            deadline = esp_timer_get_time() + 5000000;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(fd, &write_set);
            struct timeval timeout = {.tv_usec = 200000};
            select(fd + 1, NULL, &write_set, NULL, &timeout);
        } else return false;
    }
    return length == 0;
}

static void message(int fd, int status, const char *text, bool head)
{
    char header[256];
    int n = snprintf(header, sizeof(header), "HTTP/1.0 %d PSP Router\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n\r\n", status, (unsigned)strlen(text));
    if (n > 0 && n < sizeof(header) && send_all(fd, header, n, 0) && !head) {
        send_all(fd, text, strlen(text), 0);
    }
}

static int read_header(int fd, char *buffer)
{
    size_t used = 0;
    int64_t deadline = esp_timer_get_time() + 10000000;
    buffer[0] = 0;
    while (used < PROXY_HEADER_MAX && esp_timer_get_time() < deadline) {
        int n = recv(fd, buffer + used, PROXY_HEADER_MAX - used, 0);
        if (n > 0) {
            if (memchr(buffer + used, 0, n)) return -400;
            used += n;
            buffer[used] = 0;
            char *end = strstr(buffer, "\r\n\r\n");
            if (end) return end - buffer + 4;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            struct timeval timeout = {.tv_usec = 200000};
            select(fd + 1, &set, NULL, NULL, &timeout);
        } else return -400;
    }
    return used == PROXY_HEADER_MAX ? -431 : -408;
}

static void copy_header(char *target, size_t capacity, const char *value, response_meta_t *meta)
{
    size_t length = strlen(value);
    if (length >= capacity || strchr(value, '\r') || strchr(value, '\n')) {
        meta->bad = true;
    } else memcpy(target, value, length + 1);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_meta_t *meta = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (!strcasecmp(event->header_key, "Content-Type")) {
            copy_header(meta->content_type, sizeof(meta->content_type), event->header_value, meta);
        } else if (!strcasecmp(event->header_key, "Location")) {
            copy_header(meta->location, sizeof(meta->location), event->header_value, meta);
        } else if (!strcasecmp(event->header_key, "Content-Encoding")) {
            copy_header(meta->encoding, sizeof(meta->encoding), event->header_value, meta);
        }
    }
    return ESP_OK;
}

static void home(int fd, bool head, router_network_state_t state)
{
    const size_t capacity = 3072;
    char *html = malloc(capacity);
    if (!html) { message(fd, 503, "Out of resources. Retry later.\n", head); return; }
    char address[16], wan[16], dns[16];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&state.lan.ip));
    snprintf(wan, sizeof(wan), IPSTR, IP2STR(&state.wan.ip));
    snprintf(dns, sizeof(dns), IPSTR, IP2STR(&state.dns));
    unsigned requests, connect_ms, total_ms, bytes;
    bool complete;
    unsigned lan_accepted, lan_requests, lan_errors;
    esp_ip4_addr_t last_lan_ip;
    portENTER_CRITICAL(&s_stats_lock);
    requests = s_stats.requests;
    connect_ms = s_stats.connect_ms;
    total_ms = s_stats.total_ms;
    bytes = s_stats.bytes;
    complete = s_stats.complete;
    lan_accepted = s_lan_stats.accepted;
    lan_requests = s_lan_stats.requests;
    lan_errors = s_lan_stats.errors;
    last_lan_ip = s_lan_stats.last_ip;
    portEXIT_CRITICAL(&s_stats_lock);
    char network[256];
    if (state.bridge_mode) {
        snprintf(network, sizeof(network), "Bridge IP: %s. USB internet: %s. Gateway: " IPSTR ". Upstream DNS: %s.",
                 address, state.online ? "configured" : "offline", IP2STR(&state.lan.gw), dns);
    } else {
        snprintf(network, sizeof(network), "USB network: %s. WAN: %s. LAN: %s. Upstream DNS: %s.",
                 state.online ? "configured" : "offline", wan, address, dns);
    }
    int n = snprintf(html, capacity, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\nCache-Control: no-store\r\n\r\n"
        "<!DOCTYPE HTML><html><head><title>PSP Router</title></head><body>"
        "<h1>PSP Router</h1><p>%s</p>"
        "<p>Network clock: %s.</p>"
        "<p>Browser proxy: %s:%d. Open http:// addresses; the router uses HTTPS to reach websites.</p>"
        "<form action='/open' method='GET'><input name='url' value='http://example.com/' size='35'>"
        "<input type='submit' value='Open'></form>"
        "<p>Simple public pages only. GET/HEAD supported; logins, modern JavaScript, WebP/AVIF and video are not adapted.</p>"
        "<p>Fetches: %u. Last: %s, %u bytes, connection %u ms, total %u ms. "
        "Connection includes DNS, TCP and TLS; total includes delivery to this browser.</p>",
        network, time(NULL) >= 1704067200 ? "ready" : "synchronizing",
        state.bridge_mode ? PSP_PROXY_HOSTNAME : address, CONFIG_PSP_PROXY_PORT,
        requests, complete ? "complete" : "not complete", bytes, connect_ms, total_ms);
    router_lan_status_t lan = router_network_lan_status();
    if (n <= 0 || (size_t)n >= capacity) goto end;
    int written = snprintf(html + n, capacity - n,
        "<h2>Wi-Fi LAN</h2><p>Interface: %s. DHCP server: %s. Client list: %s. "
        "DHCP lookup: %s. ARP lookup: %s.</p>"
        "<p>LAN TCP connections: %u. HTTP requests: %u. Invalid/incomplete requests: %u. "
        "Last LAN peer: " IPSTR ".</p><ul>",
        lan.up ? "up" : "down", lan.external_dhcp ? "computer" : lan.dhcp_running ? "running" : "stopped",
        lan.clients_valid ? "available" : "unavailable", lan.dhcp_valid ? "available" : "unavailable",
        lan.arp_valid ? "available" : "unavailable", lan_accepted, lan_requests, lan_errors,
        IP2STR(&last_lan_ip));
    if (written < 0 || (size_t)written >= capacity - n) goto end;
    n += written;
    if (state.bridge_mode) {
        written = snprintf(html + n, capacity - n,
            "<li>Name psp-proxy: %u intercepted replies, %u direct DNS replies. "
            "DHCP and default DNS are provided by the computer.</li>", lan.name_intercepted, lan.name_direct);
        if (written < 0 || (size_t)written >= capacity - n) goto end;
        n += written;
    }
    for (unsigned i = 0; i < lan.count; ++i) {
        const router_wifi_client_t *client = &lan.clients[i];
        written = snprintf(html + n, capacity - n,
            "<li>" MACSTR ": client IP " IPSTR ", ARP " IPSTR ", RSSI %d dBm</li>",
            MAC2STR(client->mac), IP2STR(&client->dhcp_ip), IP2STR(&client->arp_ip), client->rssi);
        if (written < 0 || (size_t)written >= capacity - n) goto end;
        n += written;
    }
    written = snprintf(html + n, capacity - n, "</ul></body></html>");
    if (written < 0 || (size_t)written >= capacity - n) goto end;
    n += written;
    {
        if (head) {
            char *end = strstr(html, "\r\n\r\n");
            if (end) n = end - html + 4;
        }
        send_all(fd, html, n, 0);
    }
end:
    free(html);
}

static void local_request(int fd, proxy_request_t *request, router_network_state_t state)
{
    if (!strcmp(request->path, "/") || !strncmp(request->path, "/?", 2)) {
        home(fd, request->head, state);
        return;
    }
    if (strncmp(request->path, "/open?url=", 10)) {
        message(fd, 404, "Not found. Open the proxy home page.\n", request->head);
        return;
    }
    char *decoded = malloc(PROXY_URL_MAX);
    if (!decoded) { message(fd, 503, "Out of resources.\n", request->head); return; }
    int error = !proxy_decode_query(request->path + 10, decoded, PROXY_URL_MAX) ? 400 :
                proxy_https_url(decoded, request);
    free(decoded);
    if (error) { message(fd, error, "Enter an http:// or https:// website address.\n", request->head); return; }
    char *header = malloc(PROXY_URL_MAX + 128);
    if (header) {
        int n = snprintf(header, PROXY_URL_MAX + 128, "HTTP/1.0 302 Found\r\nLocation: http://%s\r\n"
                         "Content-Length: 0\r\nConnection: close\r\n\r\n", request->url + 8);
        if (n > 0 && n < PROXY_URL_MAX + 128) send_all(fd, header, n, 0);
        free(header);
    }
}

static void fetch(int fd, proxy_request_t *request, router_network_state_t state)
{
    if (!state.online) { message(fd, 503, "USB internet is offline.\n", request->head); return; }
    if (time(NULL) < 1704067200) {
        message(fd, 503, "Waiting for network time to verify HTTPS certificates. Retry shortly.\n", request->head);
        return;
    }
    response_meta_t *meta = calloc(1, sizeof(*meta));
    if (!meta) { message(fd, 503, "Out of resources.\n", request->head); return; }
    esp_http_client_config_t config = {
        .url = request->url, .timeout_ms = 6000, .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach, .event_handler = http_event, .user_data = meta,
        .buffer_size = 2048, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(meta); message(fd, 503, "Out of resources.\n", request->head); return; }
    esp_http_client_set_method(client, request->head ? HTTP_METHOD_HEAD : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_header(client, "User-Agent", request->user_agent[0] ? request->user_agent : "PSP-Router/2.0");
    if (request->accept[0]) esp_http_client_set_header(client, "Accept", request->accept);
    int64_t start = esp_timer_get_time();
    esp_err_t result = esp_http_client_open(client, 0);
    unsigned connect_ms = (esp_timer_get_time() - start) / 1000;
    unsigned bytes = 0;
    bool complete = false;
    if (result != ESP_OK) {
        int code = 0, flags = 0;
        esp_http_client_get_and_clear_last_tls_error(client, &code, &flags);
        message(fd, 502, flags ? "Upstream HTTPS certificate was rejected.\n" :
                "Unable to establish upstream HTTPS connection.\n", request->head);
        goto done;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0 || meta->bad ||
        (meta->encoding[0] && strcasecmp(meta->encoding, "identity"))) {
        message(fd, 502, "Upstream response headers or encoding are unsupported.\n", request->head);
        goto done;
    }
    if (!request->head && content_length > BODY_LIMIT) {
        message(fd, 502, "This page exceeds the 4 MiB gateway limit.\n", false);
        goto done;
    }
    if (router_network_state().epoch != state.epoch) goto done;
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status > 599) {
        message(fd, 502, "Unsupported server response.\n", request->head);
        goto done;
    }
    char header[256];
    int n = snprintf(header, sizeof(header), "HTTP/1.0 %d Upstream\r\nConnection: close\r\n"
                     "Content-Type: %s\r\n", status,
                     meta->content_type[0] ? meta->content_type : "application/octet-stream");
    if (n < 0 || n >= sizeof(header) || !send_all(fd, header, n, state.epoch)) goto done;
    if (meta->location[0]) {
        const char *location = meta->location;
        bool https = !strncasecmp(location, "https://", 8);
        if (!send_all(fd, "Location: ", 10, state.epoch) ||
            (https && !send_all(fd, "http://", 7, state.epoch)) ||
            !send_all(fd, location + (https ? 8 : 0), strlen(location) - (https ? 8 : 0), state.epoch) ||
            !send_all(fd, "\r\n", 2, state.epoch)) goto done;
    }
    if (!send_all(fd, "\r\n", 2, state.epoch)) goto done;
    if (request->head || status == 204 || status == 304) { complete = true; goto done; }
    bool rewrite = !strncasecmp(meta->content_type, "text/html", 9) ||
                   !strncasecmp(meta->content_type, "text/css", 8) ||
                   !strncasecmp(meta->content_type, "text/javascript", 15) ||
                   !strncasecmp(meta->content_type, "application/xhtml+xml", 21) ||
                   !strncasecmp(meta->content_type, "application/javascript", 22);
    proxy_rewriter_t rewriter = {0};
    char input[1024], output[1032];
    while (bytes < BODY_LIMIT && esp_timer_get_time() - start < 60000000 &&
           router_network_state().epoch == state.epoch) {
        unsigned remaining = BODY_LIMIT - bytes;
        n = esp_http_client_read(client, input, remaining < sizeof(input) ? remaining : sizeof(input));
        if (n < 0) break;
        if (!n) {
            complete = esp_http_client_is_complete_data_received(client);
            if (rewrite) {
                size_t tail = proxy_rewrite_finish(&rewriter, output);
                complete = send_all(fd, output, tail, state.epoch) && complete;
            }
            break;
        }
        bytes += n;
        size_t count = rewrite ? proxy_rewrite(&rewriter, input, n, output) : n;
        if (!send_all(fd, rewrite ? output : input, count, state.epoch)) break;
        if (esp_http_client_is_complete_data_received(client)) {
            complete = true;
            if (rewrite) {
                size_t tail = proxy_rewrite_finish(&rewriter, output);
                complete = send_all(fd, output, tail, state.epoch);
            }
            break;
        }
    }
done:
    esp_http_client_cleanup(client);
    free(meta);
    portENTER_CRITICAL(&s_stats_lock);
    ++s_stats.requests;
    s_stats.bytes = bytes;
    s_stats.connect_ms = connect_ms;
    s_stats.total_ms = (esp_timer_get_time() - start) / 1000;
    s_stats.complete = complete;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void worker(void *arg)
{
    connection_t *connection = arg;
    int fd = connection->fd;
    char *header = malloc(PROXY_HEADER_MAX + 1);
    proxy_request_t *request = calloc(1, sizeof(*request));
    if (!header || !request) { message(fd, 503, "Out of resources.\n", false); goto end; }
    int length = read_header(fd, header);
    int error = length < 0 ? -length : proxy_parse_request(header, length, request);
    if (connection->lan) {
        portENTER_CRITICAL(&s_stats_lock);
        if (error) ++s_lan_stats.errors;
        else ++s_lan_stats.requests;
        portEXIT_CRITICAL(&s_stats_lock);
    }
    if (error) {
        message(fd, error, "Only GET/HEAD with http:// addresses are supported. CONNECT keeps PSP TLS and is unavailable.\n", request->head);
        goto end;
    }
    router_network_state_t state = router_network_state();
    if (state.epoch != connection->epoch) { message(fd, 503, "Network changed. Retry.\n", request->head); goto end; }
    char address[16];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&state.lan.ip));
    bool local = request->local || !strcmp(request->host, address);
    snprintf(address, sizeof(address), IPSTR, IP2STR(&state.wan.ip));
    local = local || !strcmp(request->host, address);
    local = local || (state.bridge_mode && bridge_dns_hostname(request->host));
    if (local) local_request(fd, request, state);
    else fetch(fd, request, state);
end:
    free(header);
    free(request);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    free(connection);
    xSemaphoreGive(s_slots);
    vTaskDelete(NULL);
}

static void server(void *arg)
{
    int listener = -1;
    uint32_t bound = 0;
    for (;;) {
        router_network_state_t state = router_network_state();
        if (!state.lan.ip.addr || !state.lan.netmask.addr) {
            if (listener >= 0) close(listener);
            listener = -1;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (listener < 0 || bound != state.lan.ip.addr) {
            if (listener >= 0) close(listener);
            listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(CONFIG_PSP_PROXY_PORT),
                                         .sin_addr.s_addr = INADDR_ANY};
            int reuse = 1;
            if (listener >= 0) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 2)) {
                if (listener >= 0) close(listener);
                listener = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            if (fcntl(listener, F_SETFL, O_NONBLOCK) < 0) {
                close(listener);
                listener = -1;
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            bound = state.lan.ip.addr;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        struct timeval timeout = {.tv_usec = 250000};
        int ready = select(listener + 1, &set, NULL, NULL, &timeout);
        if (ready < 0) {
            close(listener);
            listener = -1;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!ready) continue;
        struct sockaddr_in peer;
        socklen_t size = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &size);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(listener);
                listener = -1;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); continue; }
        if (!router_client_allowed(peer.sin_addr.s_addr, bound, state.lan.netmask.addr, state.wan.gw.addr)) {
            close(fd);
            continue;
        }
        bool lan = (peer.sin_addr.s_addr & state.lan.netmask.addr) ==
                   (bound & state.lan.netmask.addr);
        if (lan) {
            portENTER_CRITICAL(&s_stats_lock);
            ++s_lan_stats.accepted;
            s_lan_stats.last_ip.addr = peer.sin_addr.s_addr;
            portEXIT_CRITICAL(&s_stats_lock);
        }
        if (xSemaphoreTake(s_slots, 0) != pdTRUE) {
            message(fd, 503, "Two requests are already active. Retry shortly.\n", false);
            close(fd);
            continue;
        }
        connection_t *connection = malloc(sizeof(*connection));
        if (connection) *connection = (connection_t){.fd = fd, .epoch = state.epoch, .lan = lan};
        if (!connection || xTaskCreate(worker, "browser_fetch", 12288, connection, 4, NULL) != pdPASS) {
            free(connection);
            close(fd);
            xSemaphoreGive(s_slots);
        }
    }
}

void browser_proxy_start(void)
{
    s_slots = xSemaphoreCreateCounting(PROXY_CLIENTS, PROXY_CLIENTS);
    ESP_ERROR_CHECK(s_slots ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(server, "browser_proxy", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
#endif
