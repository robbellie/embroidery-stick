/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* True if the host hasn't issued a USB read in the last few seconds.
 * Implemented in app_main.c, which is the only place that sees
 * tud_msc_read10_cb() calls. Used to pace background work (the SD-cache
 * eager-fill task, the pre-refresh wait) so it doesn't compete with the
 * embroidery machine's own reads. */
bool app_usb_is_idle(void);
