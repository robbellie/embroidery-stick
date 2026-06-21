/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Broadcasts a UDP discovery request for the backend and waits for a
 * reply, retrying up to CONFIG_EMBROIDERY_DISCOVERY_RETRIES times
 * (CONFIG_EMBROIDERY_DISCOVERY_TIMEOUT_MS apart). On success fills
 * host_out (dotted-decimal) and port_out and returns ESP_OK;
 * ESP_ERR_TIMEOUT if nothing replied.
 */
esp_err_t app_discovery_find_backend(char *host_out, size_t host_size, uint16_t *port_out);
