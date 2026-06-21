/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * LRU chunk cache backed by PSRAM.
 *
 * Keyed by (file_id, chunk_index).  chunk_index = byte_offset / chunk_size.
 * On a cache miss the cache queues an async fetch; the caller should stall
 * (return SCSI NOT READY / BECOMING READY) and retry on the next host attempt.
 */

/* Callback the cache uses to fetch data from the backend */
typedef esp_err_t (*cache_fetch_fn_t)(uint16_t file_id, uint32_t offset,
                                       uint32_t length, uint8_t *buf,
                                       uint32_t *actual_out);

esp_err_t cache_init(cache_fetch_fn_t fetch_fn, uint32_t chunk_size, uint16_t num_slots);
void      cache_deinit(void);
void      cache_invalidate_all(void);

/*
 * Look up cached data for (file_id, byte_offset).
 *
 * On hit:  sets *data_out and *available_out, returns true.
 *          *available_out is bytes available from *data_out within the chunk
 *          (may be < requested if it's the last chunk of the file).
 * On miss: queues an async fetch, returns false.
 *          Caller must stall and let the host retry.
 */
bool cache_get(uint16_t file_id, uint32_t offset,
               uint8_t **data_out, uint32_t *available_out);
