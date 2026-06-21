/* SPDX-License-Identifier: Apache-2.0 */
#include "app_cache.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cache";

typedef struct {
    uint16_t file_id;
    uint32_t chunk_index;
    uint32_t data_size;     /* actual bytes filled (may be < chunk_size for last chunk) */
    uint8_t *data;          /* chunk_size bytes */
    uint32_t lru_tick;
    bool     valid;
    bool     fetch_pending; /* fetch queued but not yet complete */
} cache_slot_t;

typedef struct {
    uint16_t file_id;
    uint32_t chunk_index;
} fetch_req_t;

static cache_slot_t    *s_slots      = NULL;
static uint16_t         s_num_slots  = 0;
static uint32_t         s_chunk_size = 0;
static cache_fetch_fn_t s_fetch_fn   = NULL;
static QueueHandle_t    s_queue      = NULL;
static SemaphoreHandle_t s_mutex     = NULL;
static uint32_t         s_lru_tick   = 0;
static TaskHandle_t     s_task       = NULL;

/* Find slot holding (file_id, chunk_index), valid or pending. Called with mutex held. */
static cache_slot_t *find_slot(uint16_t file_id, uint32_t chunk_index)
{
    for (uint16_t i = 0; i < s_num_slots; i++) {
        if ((s_slots[i].valid || s_slots[i].fetch_pending) &&
            s_slots[i].file_id == file_id &&
            s_slots[i].chunk_index == chunk_index) {
            return &s_slots[i];
        }
    }
    return NULL;
}

/* Find a free (inactive) slot. Never evicts pending. Called with mutex held. */
static cache_slot_t *find_free_slot(void)
{
    for (uint16_t i = 0; i < s_num_slots; i++) {
        if (!s_slots[i].valid && !s_slots[i].fetch_pending) {
            return &s_slots[i];
        }
    }
    /* Evict LRU valid slot */
    cache_slot_t *lru = NULL;
    for (uint16_t i = 0; i < s_num_slots; i++) {
        if (s_slots[i].valid && !s_slots[i].fetch_pending) {
            if (!lru || s_slots[i].lru_tick < lru->lru_tick) {
                lru = &s_slots[i];
            }
        }
    }
    if (lru) {
        lru->valid = false;
        return lru;
    }
    return NULL; /* all slots pending, can't evict */
}

/* Queue a fetch for (file_id, chunk_index) if no slot exists. Called with mutex held. */
static void maybe_queue_fetch(uint16_t file_id, uint32_t chunk_index)
{
    if (find_slot(file_id, chunk_index)) return; /* already cached or pending */
    cache_slot_t *slot = find_free_slot();
    if (!slot) return;

    slot->file_id      = file_id;
    slot->chunk_index  = chunk_index;
    slot->valid        = false;
    slot->fetch_pending = true;
    slot->lru_tick     = 0;

    fetch_req_t req = { .file_id = file_id, .chunk_index = chunk_index };
    xQueueSend(s_queue, &req, 0); /* non-blocking — queue depth 32 */
}

static void fetch_task(void *arg)
{
    fetch_req_t req;
    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        cache_slot_t *slot = find_slot(req.file_id, req.chunk_index);
        if (!slot || slot->valid) {
            /* Already valid (duplicate) or slot was stolen — skip */
            xSemaphoreGive(s_mutex);
            continue;
        }
        xSemaphoreGive(s_mutex);

        uint32_t offset = req.chunk_index * s_chunk_size;
        uint32_t actual = 0;
        esp_err_t err = s_fetch_fn(req.file_id, offset, s_chunk_size, slot->data, &actual);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        /* Re-check slot (might have been invalidated during fetch) */
        if (slot->fetch_pending && !slot->valid) {
            if (err == ESP_OK && actual > 0) {
                slot->data_size    = actual;
                slot->valid        = true;
                slot->fetch_pending = false;
                slot->lru_tick     = ++s_lru_tick;
                ESP_LOGD(TAG, "fetched id=%u chunk=%lu size=%lu",
                         req.file_id, req.chunk_index, actual);
            } else {
                slot->valid        = false;
                slot->fetch_pending = false;
                ESP_LOGW(TAG, "fetch failed id=%u chunk=%lu err=%d",
                         req.file_id, req.chunk_index, err);
            }
        }
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t cache_init(cache_fetch_fn_t fetch_fn, uint32_t chunk_size, uint16_t num_slots)
{
    s_fetch_fn   = fetch_fn;
    s_chunk_size = chunk_size;
    s_num_slots  = num_slots;

    s_slots = calloc(num_slots, sizeof(cache_slot_t));
    if (!s_slots) return ESP_ERR_NO_MEM;

    for (uint16_t i = 0; i < num_slots; i++) {
        s_slots[i].data = heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slots[i].data) {
            s_slots[i].data = malloc(chunk_size);
        }
        if (!s_slots[i].data) {
            ESP_LOGE(TAG, "Failed to allocate slot %u", i);
            return ESP_ERR_NO_MEM;
        }
    }

    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(32, sizeof(fetch_req_t));
    if (!s_mutex || !s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(fetch_task, "cache_fetch", 8192, NULL, 5, &s_task) != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "initialized: %u slots × %lu KB = %lu KB",
             num_slots, chunk_size / 1024, (uint32_t)num_slots * chunk_size / 1024);
    return ESP_OK;
}

void cache_deinit(void)
{
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    if (s_slots) {
        for (uint16_t i = 0; i < s_num_slots; i++) {
            free(s_slots[i].data);
        }
        free(s_slots);
        s_slots = NULL;
    }
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
}

void cache_invalidate_all(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < s_num_slots; i++) {
        s_slots[i].valid        = false;
        s_slots[i].fetch_pending = false;
    }
    xSemaphoreGive(s_mutex);
    /* Drain pending fetch queue */
    fetch_req_t req;
    while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {}
    ESP_LOGI(TAG, "invalidated");
}

bool cache_get(uint16_t file_id, uint32_t offset, uint8_t **data_out, uint32_t *available_out)
{
    uint32_t chunk_index     = offset / s_chunk_size;
    uint32_t offset_in_chunk = offset % s_chunk_size;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_slot_t *slot = find_slot(file_id, chunk_index);

    if (slot && slot->valid) {
        /* Cache HIT */
        slot->lru_tick  = ++s_lru_tick;
        *data_out       = slot->data + offset_in_chunk;
        *available_out  = (slot->data_size > offset_in_chunk)
                          ? (slot->data_size - offset_in_chunk) : 0;
        /* Prefetch next chunk while we have the mutex */
        maybe_queue_fetch(file_id, chunk_index + 1);
        xSemaphoreGive(s_mutex);
        return true;
    }

    /* Cache MISS: queue async fetch if not already pending */
    if (!slot) {
        maybe_queue_fetch(file_id, chunk_index);
    }
    /* else: slot exists with fetch_pending=true — fetch already in flight */

    xSemaphoreGive(s_mutex);
    return false; /* not ready; caller should retry (TUD_MSC_RET_BUSY) */
}
