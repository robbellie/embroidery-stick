/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t app_button_init(void);

/* Blocks polling the button. Returns false immediately if not pressed at
 * call time (no boot delay in the common case). If pressed, polls until
 * either released early (returns false) or held for the full hold_ms
 * (returns true). */
bool app_button_is_held(uint32_t hold_ms);
