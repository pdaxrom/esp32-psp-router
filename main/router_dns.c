#include "sdkconfig.h"
#if CONFIG_PSP_MODE_ROUTER
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "router_network.h"
#include "router_dns.h"
#include "router_policy.h"

static void dns_task(void *unused)
{
    int server = -1;
    uint32_t bound = 0;
    uint8_t request[1232], reply[1232];
    for (;;) {
        router_network_state_t state = router_network_state();
        if (bound != state.lan.ip.addr || server < 0) {
            if (server >= 0) close(server);
            server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct timeval timeout = {.tv_usec = 200000};
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53),
                                         .sin_addr.s_addr = INADDR_ANY};
            if (server < 0 || setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                bind(server, (struct sockaddr *)&address, sizeof(address))) {
                if (server >= 0) close(server);
                server = -1;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            bound = state.lan.ip.addr;
        }
        struct sockaddr_in client;
        socklen_t size = sizeof(client);
        int length = recvfrom(server, request, sizeof(request), 0, (struct sockaddr *)&client, &size);
        if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(server);
            server = -1;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (length < 12 || (request[2] & 0x80) ||
            !router_client_allowed(client.sin_addr.s_addr, bound, state.lan.netmask.addr, state.wan.gw.addr)) continue;
        int received = -1;
        int upstream = -1;
        if (state.online && state.dns.addr) {
            upstream = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in target = {.sin_family = AF_INET, .sin_port = htons(53),
                                        .sin_addr.s_addr = state.dns.addr};
            struct timeval timeout = {.tv_usec = 200000};
            if (upstream >= 0 && !setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) &&
                !connect(upstream, (struct sockaddr *)&target, sizeof(target)) &&
                send(upstream, request, length, 0) == length) {
                int64_t deadline = esp_timer_get_time() + 2000000;
                while (esp_timer_get_time() < deadline && router_network_state().epoch == state.epoch) {
                    int n = recv(upstream, reply, sizeof(reply), 0);
                    if (n >= 12 && reply[0] == request[0] && reply[1] == request[1] && (reply[2] & 0x80)) {
                        received = n;
                        break;
                    }
                }
            }
        }
        if (upstream >= 0) close(upstream);
        if (received < 0) {
            memcpy(reply, request, length);
            reply[2] = (request[2] & 1) | 0x80;
            reply[3] = 0x82; /* RA + SERVFAIL, retain question/additional records. */
            reply[6] = reply[7] = reply[8] = reply[9] = 0;
            received = length;
        }
        sendto(server, reply, received, 0, (struct sockaddr *)&client, size);
    }
}

void router_dns_start(void)
{
    ESP_ERROR_CHECK(xTaskCreate(dns_task, "lan_dns", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
#endif
