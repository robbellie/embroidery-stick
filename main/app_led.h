/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

typedef enum {
    APP_LED_STATE_OFF,
    APP_LED_STATE_PROVISIONING,   /* slow blue blink   — AP mode, waiting for setup            */
    APP_LED_STATE_CONNECTING,     /* fast amber blink  — STA connecting                        */
    APP_LED_STATE_DISCOVERING,    /* cyan blink        — WiFi up, backend not confirmed        */
    APP_LED_STATE_CONNECTED,      /* solid green       — WiFi + backend both confirmed         */
    APP_LED_STATE_ERROR,          /* solid red                                                 */
    APP_LED_STATE_OFFLINE,        /* solid magenta     — backend unreachable at boot, showing  */
                                   /* last-known files from the SD cache instead of live ones  */
} app_led_state_t;

esp_err_t app_led_init(void);
void      app_led_set_state(app_led_state_t state);

/*
 * Optional extra context (WiFi SSID, IP address, ...) shown alongside the
 * state on backends that can display text (see app_status_lcd.c). A no-op
 * on backends that can't (see app_led.c). Either argument may be NULL or
 * "" to clear that line.
 */
void      app_led_set_info(const char *line1, const char *line2);

/*
 * Optional SD-cache hit/miss counters, shown as a third line on backends
 * that can display text (see app_status_lcd.c). A no-op on backends that
 * can't (see app_led.c). Meant to be called periodically (e.g. from
 * app_main.c's version_poll_task) with app_sd_cache_get_stats()'s output.
 */
void      app_led_set_stats(uint32_t sd_hits, uint32_t net_fetches);

/*
 * Background SD-cache sync progress ("N/M files fully cached on SD"),
 * shown as a fourth line on backends that can display text (see
 * app_status_lcd.c). A no-op on backends that can't (see app_led.c).
 * Called by the eager-fill sync task (app_sd_cache.c) as it completes
 * each file, so `files_done` climbs to `files_total` once the whole
 * catalog is fully cached — the direct answer to "is it fully synced?"
 * that the hit/miss counters alone don't give.
 */
void      app_led_set_sync_progress(uint16_t files_done, uint16_t files_total);
