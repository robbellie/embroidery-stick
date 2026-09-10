/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "embroidery_protocol.h"

/*
 * SD card write-through cache. Mounts a FAT-formatted SD card over SPI
 * (pins from Kconfig) and caches fetched file chunks there, so a repeat
 * read is served from local flash instead of round-tripping the network.
 *
 * Only built when CONFIG_EMBROIDERY_SD_CACHE is enabled (see
 * main/CMakeLists.txt) — a no-op concept entirely on boards without a
 * card slot, like the AtomS3U.
 */

typedef enum {
    APP_SD_CACHE_ABSENT,        /* no card detected */
    APP_SD_CACHE_NEEDS_FORMAT,  /* a card responds, but isn't a readable FAT filesystem */
    APP_SD_CACHE_READY,         /* mounted and usable */
} app_sd_cache_state_t;

/* Probes and mounts the card, never formatting. ESP_OK if mounted;
 * otherwise callers should treat it as "no SD cache available" and fall
 * back to network-only, not as a fatal error — check
 * app_sd_cache_get_state() to tell "no card" apart from "card present but
 * needs formatting" for UI purposes. */
esp_err_t             app_sd_cache_init(void);
app_sd_cache_state_t  app_sd_cache_get_state(void);
bool                  app_sd_cache_present(void); /* true iff state == READY */

/*
 * Formats the card and mounts it fresh. Destructive — erases whatever is
 * on the card, not just the cache directory, since a card in
 * APP_SD_CACHE_NEEDS_FORMAT state has no filesystem this code can trust
 * enough to partially preserve. Only call this from an explicit,
 * deliberate user gesture (see app_button_watch.c) — never
 * automatically, since a card handed to the stick isn't necessarily
 * blank.
 */
esp_err_t app_sd_cache_format_and_remount(void);

/* Cumulative counts since boot: how many sd_cache_fetch() calls were
 * served from SD versus fell through to the network. Either pointer may
 * be NULL. Meant for surfacing on-screen (see app_led_set_stats()) since
 * the serial console isn't reliably available on boards that need this. */
void app_sd_cache_get_stats(uint32_t *hits, uint32_t *misses);

/*
 * Records the current catalog (from the latest LIST_FILES / do_usb_refresh)
 * so cache lookups can resolve a wire file_id to a stable identity. This
 * is required, not optional: file_id is just the backend's sorted-array
 * index, reassigned on every single catalog reload (see backend/main.go's
 * catalog.reload()) — adding or removing any file shifts every subsequent
 * file's file_id. A cache keyed by file_id would silently serve the wrong
 * file's bytes after a reload; keying by (name, size, mtime) instead,
 * which this call captures, avoids that.
 *
 * Also prunes cached files no longer in the catalog, and kicks a
 * low-priority background task that eagerly fills in any chunk not
 * already cached, so normal on-demand reads already hit SD instead of
 * needing a first cold fetch per file. Takes its own copy of files.
 */
void app_sd_cache_set_catalog(const proto_file_info_t *files, uint16_t count);

/*
 * Blocks until the eager-fill sync pass triggered by the most recent
 * app_sd_cache_set_catalog() call finishes (or is abandoned for a newer
 * one), or until timeout_ms elapses — whichever first. Returns true if the
 * pass actually finished/was abandoned, false on timeout. A no-op (returns
 * true immediately) when SD caching isn't active.
 *
 * do_usb_refresh() (app_main.c) calls this after app_sd_cache_set_catalog()
 * but before reasserting VBUS, so the real SD-card + network I/O this
 * triggers happens entirely while the drive is detached from the host —
 * never concurrently with USB-MSC enumeration/reads.
 */
bool app_sd_cache_wait_for_sync(uint32_t timeout_ms);

/*
 * Loads the last catalog persisted by app_sd_cache_set_catalog(), for use
 * when the backend is unreachable at boot — lets the stick present its
 * last-known files (as far as they're actually cached — see
 * sd_cache_fetch()) instead of an empty disk. Allocates *files_out;
 * caller frees it, matching backend_list_files()'s convention so the
 * result can be passed straight to do_usb_refresh().
 *
 * Returns ESP_ERR_NOT_FOUND if nothing has ever been synced (no catalog
 * file, or an empty one), ESP_ERR_INVALID_STATE if the SD cache isn't
 * mounted or the file is unreadable/corrupt.
 */
esp_err_t app_sd_cache_load_catalog(proto_file_info_t **files_out, uint16_t *count_out);

/*
 * Fetch function matching cache_fetch_fn_t (app_cache.h) — pass this to
 * cache_init() instead of backend_read_file() when SD caching is enabled.
 * Serves from SD on a hit; on a miss, fetches over the network via
 * backend_read_file() and writes the result through to SD before
 * returning it, so the next request for the same chunk is a local hit.
 */
esp_err_t sd_cache_fetch(uint16_t file_id, uint32_t offset, uint32_t length,
                          uint8_t *buf, uint32_t *actual_out);
