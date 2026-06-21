/* SPDX-License-Identifier: Apache-2.0 */
#include "app_button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "button";

#define BUTTON_GPIO     CONFIG_EMBROIDERY_BUTTON_GPIO
#define POLL_INTERVAL_MS 20

/* Active-low: pressed reads 0. */
static inline bool pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == 0;
}

esp_err_t app_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool app_button_is_held(uint32_t hold_ms)
{
    if (!pressed()) {
        return false;
    }

    ESP_LOGI(TAG, "button pressed at boot, checking hold duration");
    uint32_t elapsed = 0;
    while (elapsed < hold_ms) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        elapsed += POLL_INTERVAL_MS;
        if (!pressed()) {
            ESP_LOGI(TAG, "button released early (%lu ms)", (unsigned long)elapsed);
            return false;
        }
    }
    ESP_LOGI(TAG, "button held for %lu ms — forcing provisioning", (unsigned long)hold_ms);
    return true;
}
