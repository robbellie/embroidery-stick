/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

typedef enum {
    APP_LED_STATE_OFF,
    APP_LED_STATE_PROVISIONING,   /* slow blue blink  — AP mode, waiting for setup   */
    APP_LED_STATE_CONNECTING,     /* fast amber blink — STA connecting               */
    APP_LED_STATE_CONNECTED,      /* solid green                                     */
    APP_LED_STATE_ERROR,          /* solid red                                       */
} app_led_state_t;

esp_err_t app_led_init(void);
void      app_led_set_state(app_led_state_t state);
