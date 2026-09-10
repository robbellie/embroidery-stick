/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

/*
 * Watches the physical button at runtime for two gestures sharing one
 * button and one threshold ladder:
 *
 *  - Hold >= EMBROIDERY_BUTTON_HOLD_MS: reset WiFi/backend provisioning
 *    (reboots into the same provisioning flow as the boot-time
 *    app_button_is_held() check in app_main()). This is the runtime-safe
 *    equivalent of that boot-time gesture — needed because on boards
 *    where the button shares GPIO0 with the BOOT/download-mode strap
 *    (e.g. the ESP32-S3-GEEK), holding it *during* power-on/reset makes
 *    the chip enter UART download mode instead of ever reaching
 *    app_main(), so the boot-time check never gets a chance to run.
 *    Holding it any time *after* boot sidesteps that: by then GPIO0 is
 *    long past the point where the ROM bootloader samples it.
 *
 *  - Hold >= EMBROIDERY_SD_FORMAT_HOLD_MS (only offered while
 *    app_sd_cache_get_state() reports APP_SD_CACHE_NEEDS_FORMAT, and only
 *    built when both CONFIG_EMBROIDERY_SD_CACHE and
 *    CONFIG_EMBROIDERY_STATUS_BACKEND_LCD are enabled — this is
 *    destructive, so it's only offered where the countdown/warning can
 *    be shown on screen): erase and reformat the card.
 *
 * Since EMBROIDERY_SD_FORMAT_HOLD_MS > EMBROIDERY_BUTTON_HOLD_MS, a card
 * needing format doesn't remove the ability to reset WiFi — crossing the
 * WiFi-reset threshold while a card needs formatting waits rather than
 * committing immediately, so releasing before the format threshold
 * commits the WiFi reset instead, and continuing to hold commits the
 * format. Runs unconditionally on every board — a harmless single-gesture
 * path on boards without SD caching, like the AtomS3U, where the
 * boot-time gesture already works fine on its own dedicated
 * (non-strapping) pin.
 */
esp_err_t app_button_watch_start(void);
