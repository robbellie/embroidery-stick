/* SPDX-License-Identifier: Apache-2.0 */
#include "app_provision.h"
#include "app_wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "provision";

#define DNS_PORT          53
#define DNS_MAX_LEN       256
#define DNS_OPCODE_MASK   0x7800
#define DNS_QR_FLAG       (1 << 7)
#define DNS_ANSWER_TTL    300

#define SAVE_BODY_MAX     256

extern const char provision_form_start[] asm("_binary_provision_form_html_start");
extern const char provision_form_end[]   asm("_binary_provision_form_html_end");

static struct {
    wifi_ap_record_t scan_records[CONFIG_EMBROIDERY_PROVISION_MAX_SCAN];
    uint16_t          scan_count;
    bool              scanned;
} s;

/* ---- WiFi AP + scan ----------------------------------------------------- */

/* esp_netif_init()/esp_event_loop_create_default()/esp_wifi_init() and the
 * STA netif are already set up by app_main() before this is ever called —
 * doing it again here would panic (esp_wifi_init() called twice). */
static void softap_start(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, CONFIG_EMBROIDERY_PROVISION_AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(CONFIG_EMBROIDERY_PROVISION_AP_SSID);

    if (strlen(CONFIG_EMBROIDERY_PROVISION_AP_PASSWORD) > 0) {
        strlcpy((char *)ap_config.ap.password, CONFIG_EMBROIDERY_PROVISION_AP_PASSWORD, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);
    char ip_str[16];
    esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "provisioning AP '%s' up, IP=%s", CONFIG_EMBROIDERY_PROVISION_AP_SSID, ip_str);
}

static void provision_scan(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        s.scan_count = 0;
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    uint16_t count = (found < CONFIG_EMBROIDERY_PROVISION_MAX_SCAN) ? found : CONFIG_EMBROIDERY_PROVISION_MAX_SCAN;
    if (count > 0) {
        esp_wifi_scan_get_ap_records(&count, s.scan_records);
    }
    s.scan_count = count;
    ESP_LOGI(TAG, "scan found %u networks (showing %u)", found, count);
}

/* ---- DNS wildcard responder (captive-portal redirect) -------------------- */
/* Adapted from esp-idf examples/protocols/http_server/captive_portal —
 * trimmed to "answer every A query with our own IP", no entry table. */

typedef struct __attribute__((__packed__)) {
    uint16_t id, flags, qd_count, an_count, ns_count, ar_count;
} dns_header_t;

typedef struct {
    uint16_t type, class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset, type, class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

static char *skip_dns_name(char *p)
{
    while (*p != 0) {
        p += (uint8_t)*p + 1;
    }
    return p + 1;
}

static int build_dns_reply(const char *req, size_t req_len, char *reply, size_t reply_max, uint32_t ip_addr)
{
    if (req_len < sizeof(dns_header_t) || req_len > reply_max) {
        return -1;
    }
    memcpy(reply, req, req_len);

    dns_header_t *header = (dns_header_t *)reply;
    if ((ntohs(header->flags) & DNS_OPCODE_MASK) != 0) {
        return 0; /* not a standard query, ignore */
    }
    header->flags = htons((uint16_t)(ntohs(header->flags) | DNS_QR_FLAG));

    uint16_t qd_count = ntohs(header->qd_count);
    header->an_count = htons(qd_count);

    size_t reply_len = req_len + (size_t)qd_count * sizeof(dns_answer_t);
    if (reply_len > reply_max) {
        return -1;
    }

    char *cur_qd_ptr  = reply + sizeof(dns_header_t);
    char *cur_ans_ptr = reply + req_len;

    for (uint16_t i = 0; i < qd_count; i++) {
        uint16_t name_offset = (uint16_t)(cur_qd_ptr - reply);
        char *after_name = skip_dns_name(cur_qd_ptr);
        dns_question_t *question = (dns_question_t *)after_name;

        dns_answer_t *answer = (dns_answer_t *)cur_ans_ptr;
        answer->ptr_offset = htons((uint16_t)(0xC000 | name_offset));
        answer->type       = question->type;
        answer->class      = question->class;
        answer->ttl        = htonl(DNS_ANSWER_TTL);
        answer->addr_len   = htons(sizeof(ip_addr));
        answer->ip_addr    = ip_addr;

        cur_ans_ptr += sizeof(dns_answer_t);
        cur_qd_ptr   = after_name + sizeof(dns_question_t);
    }
    return (int)reply_len;
}

static void dns_redirect_task(void *arg)
{
    char rx_buffer[DNS_MAX_LEN];
    char reply[DNS_MAX_LEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket create failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS redirect responder bound on port %d", DNS_PORT);

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                            (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "DNS recvfrom failed: errno %d", errno);
            continue;
        }
        int reply_len = build_dns_reply(rx_buffer, (size_t)len, reply, sizeof(reply), ip_info.ip.addr);
        if (reply_len > 0) {
            sendto(sock, reply, reply_len, 0, (struct sockaddr *)&source_addr, socklen);
        }
    }
}

/* ---- HTTP server ---------------------------------------------------------- */

static const char *find_marker(const char *buf, size_t len, const char *marker)
{
    size_t mlen = strlen(marker);
    if (mlen > len) {
        return NULL;
    }
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(buf + i, marker, mlen) == 0) {
            return buf + i;
        }
    }
    return NULL;
}

static void escape_html(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 6 < out_size; i++) {
        switch (in[i]) {
        case '&':  o += snprintf(out + o, out_size - o, "&amp;");  break;
        case '<':  o += snprintf(out + o, out_size - o, "&lt;");   break;
        case '>':  o += snprintf(out + o, out_size - o, "&gt;");   break;
        case '"':  o += snprintf(out + o, out_size - o, "&quot;"); break;
        default:   out[o++] = in[i]; break;
        }
    }
    out[o] = '\0';
}

static esp_err_t send_portal_page(httpd_req_t *req);

static esp_err_t root_get_handler(httpd_req_t *req)
{
    /* Retry automatically if the previous scan came up empty (e.g. it ran
     * too soon after the AP came up) — avoids getting stuck showing a
     * permanently empty dropdown for the rest of this boot. */
    if (!s.scanned || s.scan_count == 0) {
        provision_scan();
        s.scanned = true;
    }
    return send_portal_page(req);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    /* Explicit forced rescan, regardless of cache state. */
    provision_scan();
    s.scanned = true;

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t send_portal_page(httpd_req_t *req)
{
    size_t total_len = (size_t)(provision_form_end - provision_form_start);
    const char *marker = "{{SSID_OPTIONS}}";
    const char *marker_pos = find_marker(provision_form_start, total_len, marker);

    httpd_resp_set_type(req, "text/html");

    if (!marker_pos) {
        httpd_resp_send(req, provision_form_start, total_len);
        return ESP_OK;
    }

    size_t head_len = (size_t)(marker_pos - provision_form_start);
    httpd_resp_send_chunk(req, provision_form_start, head_len);

    if (s.scan_count == 0) {
        httpd_resp_sendstr_chunk(req, "    <option value=\"\">(no networks found, refresh page)</option>\n");
    }
    for (uint16_t i = 0; i < s.scan_count; i++) {
        char escaped[64];
        escape_html((const char *)s.scan_records[i].ssid, escaped, sizeof(escaped));
        char option[160];
        snprintf(option, sizeof(option), "    <option value=\"%s\">%s</option>\n", escaped, escaped);
        httpd_resp_sendstr_chunk(req, option);
    }

    size_t tail_off = head_len + strlen(marker);
    httpd_resp_send_chunk(req, provision_form_start + tail_off, total_len - tail_off);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void url_decode(char *s_)
{
    char *out = s_;
    while (*s_) {
        if (*s_ == '+') {
            *out++ = ' ';
            s_++;
        } else if (*s_ == '%' && isxdigit((unsigned char)s_[1]) && isxdigit((unsigned char)s_[2])) {
            char hex[3] = { s_[1], s_[2], 0 };
            *out++ = (char)strtol(hex, NULL, 16);
            s_ += 3;
        } else {
            *out++ = *s_++;
        }
    }
    *out = '\0';
}

static esp_err_t parse_form_field(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            const char *end = strchr(val, '&');
            size_t val_len = end ? (size_t)(end - val) : strlen(val);
            if (val_len >= out_size) {
                val_len = out_size - 1;
            }
            memcpy(out, val, val_len);
            out[val_len] = '\0';
            url_decode(out);
            return ESP_OK;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void deferred_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[SAVE_BODY_MAX + 1];
    size_t to_read = (size_t)req->content_len;
    if (to_read > SAVE_BODY_MAX) {
        to_read = SAVE_BODY_MAX;
    }
    int received = httpd_req_recv(req, body, to_read);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = { 0 };
    char password[65] = { 0 };
    if (parse_form_field(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "SSID required");
        return ESP_OK;
    }
    parse_form_field(body, "password", password, sizeof(password)); /* optional, open networks */

    ESP_LOGI(TAG, "saving credentials for SSID '%s'", ssid);
    app_wifi_save_credentials(ssid, password);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Saved! The stick is restarting and will "
                            "connect to your network.</h3></body></html>");

    xTaskCreate(deferred_restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    /* iOS requires content in the response to detect a captive portal —
     * a bare redirect is not enough. */
    httpd_resp_send(req, "Redirect to the setup page", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Known OS captive-portal probe paths. Each OS expects a very specific
 * response (Apple: exact "Success" page; Android: HTTP 204; Windows:
 * exact "Microsoft Connect Test" text) to conclude there's no captive
 * portal. Serving our form directly (200 OK, different content) at these
 * exact paths is the most reliable way to make the OS pop its captive-
 * portal browser automatically — relying solely on a generic 404 redirect
 * is not reliable on modern iOS in particular. */
static const char *kCaptiveProbePaths[] = {
    "/hotspot-detect.html",       /* Apple */
    "/library/test/success.html",/* Apple (older) */
    "/generate_204",             /* Android */
    "/gen_204",                  /* Android (newer) */
    "/connecttest.txt",          /* Windows NCSI */
    "/ncsi.txt",                 /* Windows */
};

static void httpd_server_start(void)
{
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get_handler };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_get_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &save);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_handler);

    for (size_t i = 0; i < sizeof(kCaptiveProbePaths) / sizeof(kCaptiveProbePaths[0]); i++) {
        httpd_uri_t probe = { .uri = kCaptiveProbePaths[i], .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &probe);
    }

    ESP_LOGI(TAG, "captive portal HTTP server started");
}

/* ---- public API ------------------------------------------------------------ */

void app_provision_start(void)
{
    /* If app_wifi_start() already ran and is now timing out (wrong
     * password fallback), WiFi is already started in STA-only mode —
     * stop it first so softap_start() can cleanly bring it back up in
     * AP+STA mode. If we never called app_wifi_start() (no stored
     * credentials / button held), WiFi is only initialized, not started,
     * and this is a no-op. */
    if (app_wifi_is_started()) {
        esp_wifi_stop();
    }

    softap_start();
    httpd_server_start();
    xTaskCreate(dns_redirect_task, "dns_redirect", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "provisioning portal ready — connect to '%s' and open any webpage",
             CONFIG_EMBROIDERY_PROVISION_AP_SSID);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
