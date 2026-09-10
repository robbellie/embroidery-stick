/* SPDX-License-Identifier: Apache-2.0 */
#include "app_button_watch.h"
#include "app_button.h"
#include "app_led.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include <stdio.h>
#if defined(CONFIG_EMBROIDERY_SD_CACHE) && defined(CONFIG_EMBROIDERY_STATUS_BACKEND_LCD)
#include "app_sd_cache.h"
#define SD_FORMAT_GESTURE_AVAILABLE 1
#define SD_FORMAT_MS CONFIG_EMBROIDERY_SD_FORMAT_HOLD_MS
#endif

static const char *TAG = "btn_watch";

#define POLL_MS       100
#define WIFI_RESET_MS CONFIG_EMBROIDERY_BUTTON_HOLD_MS

static void request_reprovision_and_restart(void)
{
    nvs_handle_t h;
    if (nvs_open("memory", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "forceprov", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    app_led_set_state(APP_LED_STATE_PROVISIONING);
    app_led_set_info("Resetting WiFi", "rebooting...");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    /* unreachable */
}

#ifdef SD_FORMAT_GESTURE_AVAILABLE
static void do_format(void)
{
    ESP_LOGW(TAG, "button held to SD-format threshold — formatting card");
    app_led_set_info("Formatting SD", "please wait");
    esp_err_t err = app_sd_cache_format_and_remount();
    if (err == ESP_OK) {
        app_led_set_info("SD formatted", "cache ready");
    } else {
        app_led_set_info("Format failed", "check the card");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
}
#endif

/*
 * Runs one press-to-release cycle once the button goes down.
 *
 * With no SD-format ambiguity in play, WiFi reprovisioning commits the
 * instant the hold crosses WIFI_RESET_MS — same as the simple original
 * gesture, no need to wait for release.
 *
 * But the SD-format gesture shares this same button and threshold-ladder
 * (WIFI_RESET_MS < SD_FORMAT_MS by construction — see Kconfig ranges): if
 * a card currently needs formatting, crossing WIFI_RESET_MS must NOT
 * commit to a WiFi reset immediately, or there would be no way to keep
 * holding through to the format threshold. Instead it waits in that
 * window: releasing there means "I wanted the shorter action" (WiFi
 * reset, committed then), while reaching SD_FORMAT_MS while still held
 * commits the format instead. This is what guarantees a WiFi reset is
 * still reachable even with a bad card inserted — the earlier version of
 * this file fully suppressed the WiFi-reset gesture whenever a card
 * needed formatting, which left no way to reset WiFi without physically
 * removing the card.
 */
static void run_hold_session(void)
{
    uint32_t held_ms = 0;
    while (app_button_is_pressed()) {
        held_ms += POLL_MS;

#ifdef SD_FORMAT_GESTURE_AVAILABLE
        if (app_sd_cache_get_state() == APP_SD_CACHE_NEEDS_FORMAT) {
            if (held_ms >= SD_FORMAT_MS) {
                do_format();
                return;
            }
            char line2[24];
            if (held_ms < WIFI_RESET_MS) {
                snprintf(line2, sizeof(line2), "wifi reset: %us",
                         (unsigned)((WIFI_RESET_MS - held_ms + 999) / 1000));
                app_led_set_info("SD: no format", line2);
            } else {
                snprintf(line2, sizeof(line2), "keep held %us",
                         (unsigned)((SD_FORMAT_MS - held_ms + 999) / 1000));
                app_led_set_info("Release=WiFi rst", line2);
            }
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
#endif
        if (held_ms >= WIFI_RESET_MS) {
            ESP_LOGW(TAG, "button held %lu ms — requesting reprovisioning", (unsigned long)held_ms);
            request_reprovision_and_restart();
            /* unreachable */
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

#ifdef SD_FORMAT_GESTURE_AVAILABLE
    /* Released while sitting in the SD-format waiting window: a
     * deliberate "I wanted WiFi reset, not a format" signal. */
    if (held_ms >= WIFI_RESET_MS && held_ms < SD_FORMAT_MS) {
        ESP_LOGW(TAG, "button released at %lu ms during SD-format prompt — requesting reprovisioning",
                 (unsigned long)held_ms);
        request_reprovision_and_restart();
    }
#endif
}

static void watch_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (!app_button_is_pressed()) {
#ifdef SD_FORMAT_GESTURE_AVAILABLE
            if (app_sd_cache_get_state() == APP_SD_CACHE_NEEDS_FORMAT) {
                app_led_set_info("SD: no format", "hold btn to erase");
            }
#endif
            continue;
        }
        run_hold_session();
    }
}

esp_err_t app_button_watch_start(void)
{
    if (xTaskCreate(watch_task, "btn_watch", 3072, NULL, 1, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
