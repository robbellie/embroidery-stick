/* SPDX-License-Identifier: Apache-2.0 */
#include "app_backend_client.h"
#include "embroidery_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include "sdkconfig.h"

static const char *TAG = "backend";

static struct {
    char     host[128];
    uint16_t port;
    int      sock;
    SemaphoreHandle_t mutex;
} s = { .sock = -1 };

/* ---- Socket helpers ---------------------------------------------------- */

static esp_err_t recv_all(int sock, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int r = recv(sock, (char *)buf + done, len - done, 0);
        if (r <= 0) return ESP_FAIL;
        done += (size_t)r;
    }
    return ESP_OK;
}

static esp_err_t send_all(int sock, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int r = send(sock, (const char *)buf + done, len - done, 0);
        if (r <= 0) return ESP_FAIL;
        done += (size_t)r;
    }
    return ESP_OK;
}

/* ---- Connection management --------------------------------------------- */

static void disconnect(void)
{
    if (s.sock >= 0) {
        close(s.sock);
        s.sock = -1;
        ESP_LOGI(TAG, "disconnected");
    }
}

static esp_err_t connect_to_backend(void)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", s.port);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(s.host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS failed for %s", s.host);
        return ESP_FAIL;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ESP_FAIL; }

    struct timeval tv = {.tv_sec = 5,  .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect failed: %d", errno);
        close(sock);
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    freeaddrinfo(res);
    s.sock = sock;

    /* HELLO handshake */
    proto_frame_t frame = { .cmd = CMD_HELLO, .payload_len = sizeof(proto_hello_req_t) };
    proto_hello_req_t req = { .proto_version = EMBROIDERY_PROTO_VERSION };
    if (send_all(sock, &frame, sizeof(frame)) != ESP_OK ||
        send_all(sock, &req,   sizeof(req))   != ESP_OK) {
        disconnect(); return ESP_FAIL;
    }
    proto_frame_t resp_hdr;
    proto_hello_resp_t resp;
    if (recv_all(sock, &resp_hdr, sizeof(resp_hdr)) != ESP_OK || resp_hdr.cmd != CMD_HELLO ||
        recv_all(sock, &resp,     sizeof(resp))      != ESP_OK) {
        disconnect(); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connected to %s:%u (disk_version=%llu)",
             s.host, s.port, (unsigned long long)resp.disk_version);
    return ESP_OK;
}

static esp_err_t ensure_connected(void)
{
    if (s.sock >= 0) return ESP_OK;
    return connect_to_backend();
}

static esp_err_t send_frame(uint8_t cmd, const void *payload, uint32_t plen)
{
    proto_frame_t hdr = { .cmd = cmd, .payload_len = plen };
    if (send_all(s.sock, &hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
    if (plen > 0 && payload && send_all(s.sock, payload, plen) != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

/* ---- Public API -------------------------------------------------------- */

esp_err_t backend_init(const char *host, uint16_t port)
{
    strlcpy(s.host, host, sizeof(s.host));
    s.port  = port;
    s.sock  = -1;
    s.mutex = xSemaphoreCreateMutex();
    if (!s.mutex) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void backend_deinit(void)
{
    disconnect();
    if (s.mutex) { vSemaphoreDelete(s.mutex); s.mutex = NULL; }
}

esp_err_t backend_get_version(uint64_t *version_out)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (ensure_connected() != ESP_OK) { xSemaphoreGive(s.mutex); return ESP_FAIL; }

    if (send_frame(CMD_VERSION, NULL, 0) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    proto_frame_t hdr; proto_version_resp_t resp;
    if (recv_all(s.sock, &hdr, sizeof(hdr)) != ESP_OK || hdr.cmd != CMD_VERSION ||
        recv_all(s.sock, &resp, sizeof(resp)) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    *version_out = resp.disk_version;
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

esp_err_t backend_list_files(proto_file_info_t **files_out, uint16_t *count_out)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (ensure_connected() != ESP_OK) { xSemaphoreGive(s.mutex); return ESP_FAIL; }

    if (send_frame(CMD_LIST_FILES, NULL, 0) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    proto_frame_t hdr;
    if (recv_all(s.sock, &hdr, sizeof(hdr)) != ESP_OK || hdr.cmd != CMD_LIST_FILES) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    uint16_t count;
    if (recv_all(s.sock, &count, sizeof(count)) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    proto_file_info_t *files = NULL;
    if (count > 0) {
        files = malloc(count * sizeof(proto_file_info_t));
        if (!files) { disconnect(); xSemaphoreGive(s.mutex); return ESP_ERR_NO_MEM; }
        if (recv_all(s.sock, files, count * sizeof(proto_file_info_t)) != ESP_OK) {
            free(files); disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
        }
    }
    *files_out = files;
    *count_out = count;
    ESP_LOGI(TAG, "list_files: %u files", count);
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

esp_err_t backend_read_file(uint16_t file_id, uint32_t offset,
                             uint32_t length, uint8_t *buf, uint32_t *actual_out)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    if (ensure_connected() != ESP_OK) { xSemaphoreGive(s.mutex); return ESP_FAIL; }

    proto_read_req_t req = { .file_id = file_id, .offset = offset, .length = length };
    if (send_frame(CMD_READ_FILE, &req, sizeof(req)) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    proto_frame_t hdr; uint32_t actual;
    if (recv_all(s.sock, &hdr,    sizeof(hdr))    != ESP_OK || hdr.cmd != CMD_READ_FILE ||
        recv_all(s.sock, &actual, sizeof(actual))  != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    if (actual > length) actual = length;
    if (recv_all(s.sock, buf, actual) != ESP_OK) {
        disconnect(); xSemaphoreGive(s.mutex); return ESP_FAIL;
    }
    *actual_out = actual;
    xSemaphoreGive(s.mutex);
    return ESP_OK;
}

/* ---- Stub mode --------------------------------------------------------- */

#ifdef CONFIG_EMBROIDERY_STUB_MODE

/* Embedded PES file (hartje_1.PES, 5 KB real borduurbestand) */
extern const uint8_t _binary_hartje_1_PES_start[];
extern const uint8_t _binary_hartje_1_PES_end[];

esp_err_t backend_stub_fetch(uint16_t file_id, uint32_t offset,
                              uint32_t length, uint8_t *buf, uint32_t *actual_out)
{
    /* All stub files serve the same embedded PES data (wraps around if larger) */
    (void)file_id;
    const uint8_t *pes_data = _binary_hartje_1_PES_start;
    uint32_t pes_size = (uint32_t)(_binary_hartje_1_PES_end - _binary_hartje_1_PES_start);

    if (offset >= pes_size) {
        *actual_out = 0;
        return ESP_OK;
    }
    uint32_t available = pes_size - offset;
    uint32_t to_copy = (length < available) ? length : available;
    memcpy(buf, pes_data + offset, to_copy);
    *actual_out = to_copy;
    return ESP_OK;
}

esp_err_t backend_stub_init(proto_file_info_t **files_out, uint16_t *count_out)
{
    uint32_t pes_size = (uint32_t)(_binary_hartje_1_PES_end - _binary_hartje_1_PES_start);

    /* Drie kopieën van het echte PES bestand met verschillende namen */
    static const char *names[] = { "HARTJE1.PES", "HARTJE2.PES", "HARTJE3.PES" };
    uint16_t n = 3;

    proto_file_info_t *files = calloc(n, sizeof(*files));
    if (!files) return ESP_ERR_NO_MEM;
    for (uint16_t i = 0; i < n; i++) {
        strlcpy(files[i].name, names[i], sizeof(files[i].name));
        files[i].size    = pes_size;
        files[i].mtime   = 1700000000u + i * 86400u;
        files[i].file_id = i;
    }
    *files_out = files;
    *count_out = n;
    ESP_LOGI(TAG, "stub: %u PES files (%lu bytes each)", n, pes_size);
    return ESP_OK;
}

#endif /* CONFIG_EMBROIDERY_STUB_MODE */
