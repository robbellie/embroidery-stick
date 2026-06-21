/* SPDX-License-Identifier: Apache-2.0 */
#include "app_discovery.h"
#include "embroidery_protocol.h"
#include "esp_log.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "sdkconfig.h"

static const char *TAG = "discovery";

esp_err_t app_discovery_find_backend(char *host_out, size_t host_size, uint16_t *port_out)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct timeval tv = {
        .tv_sec  = CONFIG_EMBROIDERY_DISCOVERY_TIMEOUT_MS / 1000,
        .tv_usec = (CONFIG_EMBROIDERY_DISCOVERY_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_EMBROIDERY_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    const char *magic = EMBROIDERY_DISCOVERY_MAGIC;
    size_t magic_len = strlen(magic);
    char buf[64];

    esp_err_t result = ESP_ERR_TIMEOUT;

    for (int attempt = 0; attempt < CONFIG_EMBROIDERY_DISCOVERY_RETRIES; attempt++) {
        if (sendto(sock, magic, magic_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
            ESP_LOGW(TAG, "broadcast send failed: errno %d", errno);
            continue;
        }

        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            continue; /* timeout or transient error — retry */
        }
        if ((size_t)len < magic_len + 2 || memcmp(buf, magic, magic_len) != 0) {
            continue; /* not our reply, ignore and keep waiting/retrying */
        }

        uint16_t port;
        memcpy(&port, buf + magic_len, sizeof(port));
        *port_out = port;

        char ip_str[16];
        inet_ntoa_r(source_addr.sin_addr, ip_str, sizeof(ip_str));
        strlcpy(host_out, ip_str, host_size);

        ESP_LOGI(TAG, "found backend at %s:%u (attempt %d/%d)",
                 host_out, port, attempt + 1, CONFIG_EMBROIDERY_DISCOVERY_RETRIES);
        result = ESP_OK;
        break;
    }

    close(sock);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "no backend found after %d attempts", CONFIG_EMBROIDERY_DISCOVERY_RETRIES);
    }
    return result;
}
