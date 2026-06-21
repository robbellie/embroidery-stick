/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t app_wifi_start(void);
void      app_wifi_wait_connected(void);
bool      app_wifi_is_connected(void);
esp_err_t app_wifi_save_credentials(const char *ssid, const char *password);
