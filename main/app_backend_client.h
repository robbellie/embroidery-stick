/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "embroidery_protocol.h"
#include "app_cache.h"   /* for cache_fetch_fn_t */

/*
 * Backend client — persistent TCP connection to the Go backend server.
 * All functions are thread-safe (mutex-protected).
 */

esp_err_t backend_init(const char *host, uint16_t port);
void      backend_deinit(void);

esp_err_t backend_get_version(uint64_t *version_out);

/*
 * Fetch file list.  Allocates *files_out; caller must free().
 */
esp_err_t backend_list_files(proto_file_info_t **files_out, uint16_t *count_out);

/*
 * Read file data.  Matches cache_fetch_fn_t — pass directly to cache_init().
 */
esp_err_t backend_read_file(uint16_t file_id, uint32_t offset,
                             uint32_t length, uint8_t *buf,
                             uint32_t *actual_out);

/* ---- Stub mode (no network) ------------------------------------------- */
#ifdef CONFIG_EMBROIDERY_STUB_MODE
esp_err_t backend_stub_init(proto_file_info_t **files_out, uint16_t *count_out);
esp_err_t backend_stub_fetch(uint16_t file_id, uint32_t offset,
                              uint32_t length, uint8_t *buf, uint32_t *actual_out);
#endif
