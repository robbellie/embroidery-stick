/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * SoftAP + captive-portal WiFi provisioning.
 *
 * Brings up an open WiFi access point with a web form (scanned SSID
 * dropdown + password field). Once the user submits credentials, saves
 * them via app_wifi_save_credentials() and reboots into normal STA mode.
 *
 * Blocks forever — never returns under normal operation.
 */
void app_provision_start(void);
