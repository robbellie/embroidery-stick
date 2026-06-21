/* SPDX-License-Identifier: Apache-2.0 */
#include "app_led.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "led";

/* Desk indicator, not a flashlight — keep it dim. */
#define LED_BRIGHTNESS 30

typedef struct {
    uint8_t r, g, b;
    uint32_t on_ms;   /* 0 = solid on */
    uint32_t off_ms;  /* ignored when on_ms == 0 */
} led_pattern_t;

static const led_pattern_t s_patterns[] = {
    [APP_LED_STATE_OFF]          = { 0,               0,               0,               0,    0   },
    [APP_LED_STATE_PROVISIONING] = { 0,               0,               LED_BRIGHTNESS,  500,  500 },
    [APP_LED_STATE_CONNECTING]   = { LED_BRIGHTNESS,  LED_BRIGHTNESS,  0,               125,  125 },
    [APP_LED_STATE_DISCOVERING]  = { 0,               LED_BRIGHTNESS,  LED_BRIGHTNESS,  300,  300 },
    [APP_LED_STATE_CONNECTED]    = { 0,               LED_BRIGHTNESS,  0,               0,    0   },
    [APP_LED_STATE_ERROR]        = { LED_BRIGHTNESS,  0,               0,               0,    0   },
};

static struct {
    led_strip_handle_t strip;
    volatile app_led_state_t state;
} s;

static void led_task(void *arg)
{
    bool on = true;
    while (1) {
        const led_pattern_t *p = &s_patterns[s.state];

        if (p->on_ms == 0) {
            /* Solid (or off) */
            led_strip_set_pixel(s.strip, 0, p->r, p->g, p->b);
            led_strip_refresh(s.strip);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (on) {
            led_strip_set_pixel(s.strip, 0, p->r, p->g, p->b);
        } else {
            led_strip_set_pixel(s.strip, 0, 0, 0, 0);
        }
        led_strip_refresh(s.strip);
        vTaskDelay(pdMS_TO_TICKS(on ? p->on_ms : p->off_ms));
        on = !on;
    }
}

esp_err_t app_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num         = CONFIG_EMBROIDERY_LED_GPIO,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out       = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s.strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %d", err);
        return err;
    }

    s.state = APP_LED_STATE_OFF;
    led_strip_clear(s.strip);

    if (xTaskCreate(led_task, "led", 2048, NULL, 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_led_set_state(app_led_state_t state)
{
    s.state = state;
}
