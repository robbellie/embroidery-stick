/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Assumes nvs_flash_init()/esp_netif_init()/esp_event_loop_create_default()
 * have already been called by app_main(). */
esp_err_t app_wifi_start(void);
void      app_wifi_wait_connected(void);

/* Like app_wifi_wait_connected() but bounded. Returns ESP_OK if connected
 * before timeout_ms elapses, ESP_ERR_TIMEOUT otherwise. */
esp_err_t app_wifi_wait_connected_timeout(uint32_t timeout_ms);

bool      app_wifi_is_connected(void);
esp_err_t app_wifi_save_credentials(const char *ssid, const char *password);

/* True if NVS has a non-empty stored SSID (or Kconfig fallback is set). */
bool      app_wifi_has_credentials(void);
