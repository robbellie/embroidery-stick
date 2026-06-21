/* SPDX-License-Identifier: Apache-2.0 */
#include "app_wifi.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_events;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying...");
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Read a string from NVS namespace "memory", with a fallback value */
static void nvs_read_or_default(const char *key, char *buf, size_t size, const char *fallback)
{
    nvs_handle_t h;
    if (nvs_open("memory", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = size;
        if (nvs_get_str(h, key, buf, &sz) != ESP_OK) {
            strlcpy(buf, fallback, size);
        }
        nvs_close(h);
    } else {
        strlcpy(buf, fallback, size);
    }
}

esp_err_t app_wifi_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_events = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, event_handler, NULL));

    char ssid[32]     = "";
    char password[64] = "";
    nvs_read_or_default("stassid",   ssid,     sizeof(ssid),     CONFIG_EMBROIDERY_WIFI_SSID);
    nvs_read_or_default("stapasswd", password, sizeof(password), CONFIG_EMBROIDERY_WIFI_PASSWORD);

    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "No SSID configured — WiFi not started");
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to SSID: %s", ssid);
    return ESP_OK;
}

void app_wifi_wait_connected(void)
{
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

bool app_wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

/* Save WiFi credentials to NVS for next boot */
esp_err_t app_wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("memory", NVS_READWRITE, &h));
    nvs_set_str(h, "stassid",   ssid);
    nvs_set_str(h, "stapasswd", password);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}
