/* SPDX-License-Identifier: Apache-2.0 */
#include "app_sd_cache.h"
#include "app_backend_client.h"
#include "app_cache.h"
#include "app_led.h"
#include "app_usb_activity.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "sd_cache";

#define SD_SPI_HOST     SPI3_HOST   /* independent from the LCD status backend's SPI2_HOST */
#define SD_MOUNT_POINT  "/sdcard"
#define SD_CACHE_DIR    SD_MOUNT_POINT "/cache"
#define SD_INDEX_PATH    SD_CACHE_DIR "/INDEX.DAT"    /* 8.3-safe name */
#define SD_CATALOG_PATH  SD_CACHE_DIR "/CATALOG.DAT"  /* last-known file list, for offline boot */
#define SD_MAX_CHUNKS    256                          /* 8KB chunks: up to 2MB tracked per file */
#define SD_INDEX_MAGIC   0x31434453u                  /* "SDC1" */
#define SD_CATALOG_MAGIC 0x31434453u                  /* "SDC1" — same tag, different file */

typedef struct __attribute__((packed)) {
    char     name[13];
    uint32_t size;
    uint32_t mtime;
    uint32_t chunk_size;
    uint16_t num_chunks;
    uint8_t  bitmap[SD_MAX_CHUNKS / 8];
} sd_cache_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
} sd_index_header_t;

static struct {
    app_sd_cache_state_t state;
    sdmmc_card_t *card;
    SemaphoreHandle_t mutex;       /* guards records[]/record_count and catalog[]/catalog_count/generation */
    SemaphoreHandle_t sync_signal;
    SemaphoreHandle_t sync_done;   /* given once per generation's sync pass finishes (or is abandoned) */
    bool sync_task_started;

    sd_cache_record_t *records;
    uint16_t record_count;

    proto_file_info_t *catalog;
    uint16_t catalog_count;
    volatile uint32_t generation;  /* bumped on every set_catalog; lets the sync task abandon a stale pass */

    volatile uint32_t hit_count;   /* sd_cache_fetch() served from SD */
    volatile uint32_t miss_count;  /* sd_cache_fetch() fell through to the network */
} s;

/* ---- index persistence -------------------------------------------------- */

static void persist_index(void)
{
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    FILE *f = fopen(SD_INDEX_PATH, "wb");
    if (f) {
        sd_index_header_t hdr = { .magic = SD_INDEX_MAGIC, .count = s.record_count };
        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite(s.records, sizeof(sd_cache_record_t), s.record_count, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "failed to persist cache index");
    }
    xSemaphoreGive(s.mutex);
}

static void load_index(void)
{
    FILE *f = fopen(SD_INDEX_PATH, "rb");
    if (!f) return; /* no index yet — starts empty, not an error */

    sd_index_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != SD_INDEX_MAGIC ||
        hdr.count > EMBROIDERY_MAX_FILES) {
        ESP_LOGW(TAG, "cache index missing/corrupt — starting empty");
        fclose(f);
        return;
    }
    s.record_count = (uint16_t)fread(s.records, sizeof(sd_cache_record_t), hdr.count, f);
    fclose(f);
    ESP_LOGI(TAG, "loaded SD cache index: %u entries", s.record_count);
}

/* ---- catalog persistence (for offline-at-boot fallback) -----------------
 * Distinct from the cache index above: this is the file *list* itself
 * (name/size/mtime/file_id), not which chunks are cached. Without this,
 * a stick powered on with the backend completely unreachable has no way
 * to know what files even exist, and would present an empty disk even
 * though the file contents might already be fully cached on SD. */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t count;
} sd_catalog_header_t;

static void persist_catalog(const proto_file_info_t *files, uint16_t count)
{
    FILE *f = fopen(SD_CATALOG_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "failed to persist catalog");
        return;
    }
    sd_catalog_header_t hdr = { .magic = SD_CATALOG_MAGIC, .count = count };
    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(files, sizeof(proto_file_info_t), count, f);
    fclose(f);
}

esp_err_t app_sd_cache_load_catalog(proto_file_info_t **files_out, uint16_t *count_out)
{
    if (s.state != APP_SD_CACHE_READY) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(SD_CATALOG_PATH, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    sd_catalog_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != SD_CATALOG_MAGIC ||
        hdr.count > EMBROIDERY_MAX_FILES) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }
    if (hdr.count == 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND; /* never synced anything — nothing to offer */
    }

    proto_file_info_t *files = malloc(sizeof(proto_file_info_t) * hdr.count);
    if (!files) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(files, sizeof(proto_file_info_t), hdr.count, f);
    fclose(f);
    if (got != hdr.count) {
        free(files);
        return ESP_ERR_INVALID_STATE;
    }

    *files_out = files;
    *count_out = hdr.count;
    return ESP_OK;
}

/* ---- record lookup/creation (caller holds s.mutex) ---------------------- */

static sd_cache_record_t *find_record_locked(const char *name)
{
    for (uint16_t i = 0; i < s.record_count; i++) {
        if (strncmp(s.records[i].name, name, sizeof(s.records[i].name)) == 0) {
            return &s.records[i];
        }
    }
    return NULL;
}

/* Finds the record for `name`, creating or resetting it if the backend
 * file's identity (size/mtime) doesn't match what's cached — an implicit,
 * lazy eviction: no explicit delete of old bytes, the bitmap reset alone
 * is enough to stop them ever being served, and new writes just overwrite
 * the old blob byte range by byte range as they land. */
static sd_cache_record_t *get_record_locked(const char *name, uint32_t size, uint32_t mtime)
{
    sd_cache_record_t *rec = find_record_locked(name);
    if (rec && rec->size == size && rec->mtime == mtime && rec->chunk_size == EMBROIDERY_CHUNK_SIZE) {
        return rec;
    }
    if (!rec) {
        if (s.record_count >= EMBROIDERY_MAX_FILES) {
            ESP_LOGW(TAG, "cache index full, dropping %s", name);
            return NULL;
        }
        rec = &s.records[s.record_count++];
    }
    memset(rec, 0, sizeof(*rec));
    strlcpy(rec->name, name, sizeof(rec->name));
    rec->size       = size;
    rec->mtime      = mtime;
    rec->chunk_size = EMBROIDERY_CHUNK_SIZE;
    uint32_t n = (size + EMBROIDERY_CHUNK_SIZE - 1) / EMBROIDERY_CHUNK_SIZE;
    rec->num_chunks = (uint16_t)(n > SD_MAX_CHUNKS ? SD_MAX_CHUNKS : n);
    return rec;
}

static bool record_fully_cached_locked(const sd_cache_record_t *rec)
{
    for (uint16_t i = 0; i < rec->num_chunks; i++) {
        if (!(rec->bitmap[i / 8] & (1 << (i % 8)))) return false;
    }
    return true;
}

/* ---- read/write-through --------------------------------------------------
 * Chunk granularity always matches EMBROIDERY_CHUNK_SIZE: the RAM cache
 * (app_cache.c) is this module's only caller via sd_cache_fetch(), and it
 * always requests exactly one full, chunk-aligned chunk per call. */

static bool try_read_from_sd(const char *name, uint32_t size, uint32_t mtime,
                              uint32_t offset, uint8_t *buf, uint32_t length, uint32_t *actual_out)
{
    uint32_t chunk_idx = offset / EMBROIDERY_CHUNK_SIZE;
    char blob_path[64];
    bool hit = false;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    sd_cache_record_t *rec = find_record_locked(name);
    if (rec && rec->size == size && rec->mtime == mtime && rec->chunk_size == EMBROIDERY_CHUNK_SIZE &&
        chunk_idx < rec->num_chunks && (rec->bitmap[chunk_idx / 8] & (1 << (chunk_idx % 8)))) {
        hit = true;
        snprintf(blob_path, sizeof(blob_path), "%s/%s", SD_CACHE_DIR, name);
    }
    xSemaphoreGive(s.mutex);
    if (!hit) return false;

    FILE *f = fopen(blob_path, "rb");
    if (!f) return false;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint32_t want = (offset + length > size) ? (size - offset) : length;
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    if (got == 0) return false;

    *actual_out = (uint32_t)got;
    return true;
}

static void write_through_to_sd(const char *name, uint32_t size, uint32_t mtime,
                                 uint32_t offset, const uint8_t *data, uint32_t actual)
{
    char blob_path[64];
    snprintf(blob_path, sizeof(blob_path), "%s/%s", SD_CACHE_DIR, name);

    FILE *f = fopen(blob_path, "r+b");
    if (!f) f = fopen(blob_path, "w+b");
    if (!f) {
        ESP_LOGW(TAG, "failed to open %s for write-through", blob_path);
        return;
    }
    fseek(f, (long)offset, SEEK_SET);
    size_t written = fwrite(data, 1, actual, f);
    fclose(f);
    if (written != actual) {
        ESP_LOGW(TAG, "short write to %s (%u/%u)", blob_path, (unsigned)written, (unsigned)actual);
        return;
    }

    bool now_complete = false;
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    sd_cache_record_t *rec = get_record_locked(name, size, mtime);
    if (rec) {
        uint32_t chunk_idx = offset / EMBROIDERY_CHUNK_SIZE;
        if (chunk_idx < rec->num_chunks) {
            rec->bitmap[chunk_idx / 8] |= (1 << (chunk_idx % 8));
        }
        now_complete = record_fully_cached_locked(rec);
    }
    xSemaphoreGive(s.mutex);

    /* Persist only on file-complete (not every chunk) — keeps SD writes
     * infrequent; worst case on power loss is re-fetching a few chunks
     * next time, never serving wrong data (the bitmap alone gates reads). */
    if (now_complete) persist_index();
}

/* ---- file_id -> stable identity resolution ------------------------------
 * file_id is just the backend's sorted-array index (backend/main.go's
 * catalog.reload()), reassigned on every reload — never used as a cache
 * key. This resolves it to the (name, size, mtime) identity from the
 * latest catalog snapshot instead. */

static bool resolve_identity_locked(uint16_t file_id, char *name_out, size_t name_size,
                                     uint32_t *size_out, uint32_t *mtime_out)
{
    for (uint16_t i = 0; i < s.catalog_count; i++) {
        if (s.catalog[i].id == file_id) {
            strlcpy(name_out, s.catalog[i].name, name_size);
            *size_out  = s.catalog[i].size;
            *mtime_out = s.catalog[i].mtime;
            return true;
        }
    }
    return false;
}

esp_err_t sd_cache_fetch(uint16_t file_id, uint32_t offset, uint32_t length,
                          uint8_t *buf, uint32_t *actual_out)
{
    /* Safe to call before any do_mount() attempt has ever run (s.mutex not
     * yet created) — lets cache_init() always be wired to this function
     * regardless of when/whether the SD card actually gets mounted, e.g.
     * when the mount is deliberately deferred until after USB is already
     * up and stable. */
    if (s.state != APP_SD_CACHE_READY) {
        return backend_read_file(file_id, offset, length, buf, actual_out);
    }

    char name[13];
    uint32_t size = 0, mtime = 0;

    xSemaphoreTake(s.mutex, portMAX_DELAY);
    bool known = resolve_identity_locked(file_id, name, sizeof(name), &size, &mtime);
    xSemaphoreGive(s.mutex);

    if (known && try_read_from_sd(name, size, mtime, offset, buf, length, actual_out)) {
        s.hit_count++;
        return ESP_OK;
    }

    s.miss_count++;
    esp_err_t err = backend_read_file(file_id, offset, length, buf, actual_out);
    if (err == ESP_OK && known && *actual_out > 0) {
        write_through_to_sd(name, size, mtime, offset, buf, *actual_out);
    }
    return err;
}

void app_sd_cache_get_stats(uint32_t *hits, uint32_t *misses)
{
    if (hits)   *hits   = s.hit_count;
    if (misses) *misses = s.miss_count;
}

/* ---- background eager-fill task ------------------------------------------
 * Low priority, deliberately paced — walks the latest catalog and fills
 * in any chunk not already cached, so an on-demand read from the
 * embroidery machine almost always finds it already on SD. Abandons the
 * current pass early if a newer catalog has arrived (generation bumped)
 * rather than racing it. */

static void sync_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        xSemaphoreTake(s.sync_signal, portMAX_DELAY);

        xSemaphoreTake(s.mutex, portMAX_DELAY);
        uint32_t my_gen   = s.generation;
        uint16_t count    = s.catalog_count;
        proto_file_info_t *snapshot = count ? malloc(sizeof(proto_file_info_t) * count) : NULL;
        if (snapshot) memcpy(snapshot, s.catalog, sizeof(proto_file_info_t) * count);
        xSemaphoreGive(s.mutex);
        if (count > 0 && !snapshot) {
            xSemaphoreGive(s.sync_done); /* real alloc failure — don't leave a waiter stuck forever */
            continue;
        }

        /* Belt-and-suspenders alongside do_usb_refresh()'s
         * app_sd_cache_wait_for_sync() call (which is the real guarantee:
         * VBUS stays down until this whole pass finishes) — this also
         * holds off real SD/network I/O for a few seconds after any USB
         * activity in the steady state, e.g. a live catalog change while
         * the drive is already mounted and being read. */
        while (!app_usb_is_idle() && s.generation == my_gen) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (s.generation != my_gen) {
            free(snapshot);
            xSemaphoreGive(s.sync_done); /* abandoned — still don't leave a waiter stuck */
            continue;
        }

        /* Pruning stale blobs (unlink) and persisting the index/catalog are
         * real SD-card I/O — deliberately done here, not in
         * app_sd_cache_set_catalog(), which runs synchronously on
         * do_usb_refresh()'s critical path while VBUS is still deasserted.
         * SD cards can stall unpredictably on writes (wear-leveling/GC
         * pauses); doing this on that critical path stretches the
         * "disconnect" window the host sees by an unbounded amount. Here,
         * VBUS is already back up by the time this runs. */
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        uint16_t kept = 0;
        for (uint16_t i = 0; i < s.record_count; i++) {
            bool still_present = false;
            for (uint16_t j = 0; j < count; j++) {
                if (strncmp(s.records[i].name, snapshot[j].name, sizeof(s.records[i].name)) == 0) {
                    still_present = true;
                    break;
                }
            }
            if (still_present) {
                if (kept != i) s.records[kept] = s.records[i];
                kept++;
            } else {
                char blob_path[64];
                snprintf(blob_path, sizeof(blob_path), "%s/%s", SD_CACHE_DIR, s.records[i].name);
                unlink(blob_path);
            }
        }
        s.record_count = kept;
        xSemaphoreGive(s.mutex);

        persist_index();
        persist_catalog(snapshot, count);

        /* Directories have no fetchable content — never enter the sync
         * loop or the hit/miss accounting, and don't count toward the
         * "N/M files cached" progress total. */
        uint16_t total_files = 0;
        for (uint16_t i = 0; i < count; i++) {
            if (!snapshot[i].is_dir) total_files++;
        }

        uint16_t files_done = 0;
        app_led_set_sync_progress(files_done, total_files);

        for (uint16_t i = 0; i < count && s.generation == my_gen; i++) {
            const proto_file_info_t *fi = &snapshot[i];
            if (fi->is_dir) continue;
            uint32_t num_chunks = (fi->size + EMBROIDERY_CHUNK_SIZE - 1) / EMBROIDERY_CHUNK_SIZE;

            for (uint32_t c = 0; c < num_chunks && s.generation == my_gen; c++) {
                /* Wait out real USB activity entirely rather than just a
                 * fixed small delay — an embroidery machine's own reads
                 * (or a host browsing the drive) should never compete with
                 * background prefetching for the shared SD/SPI bus and
                 * network connection. Only paces actual fetches; the
                 * already-cached check below still runs immediately. */
                while (!app_usb_is_idle() && s.generation == my_gen) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                if (s.generation != my_gen) break;

                uint32_t offset = c * EMBROIDERY_CHUNK_SIZE;

                xSemaphoreTake(s.mutex, portMAX_DELAY);
                sd_cache_record_t *rec = find_record_locked(fi->name);
                bool already = rec && rec->size == fi->size && rec->mtime == fi->mtime &&
                               rec->chunk_size == EMBROIDERY_CHUNK_SIZE &&
                               c < rec->num_chunks && (rec->bitmap[c / 8] & (1 << (c % 8)));
                xSemaphoreGive(s.mutex);
                if (already) continue;

                uint8_t *buf = malloc(EMBROIDERY_CHUNK_SIZE);
                if (!buf) break;
                uint32_t actual = 0;
                esp_err_t err = backend_read_file(fi->id, offset, EMBROIDERY_CHUNK_SIZE, buf, &actual);
                if (err == ESP_OK && actual > 0) {
                    write_through_to_sd(fi->name, fi->size, fi->mtime, offset, buf, actual);
                }
                free(buf);
                vTaskDelay(pdMS_TO_TICKS(20)); /* still yield a little even while idle */
            }

            xSemaphoreTake(s.mutex, portMAX_DELAY);
            sd_cache_record_t *rec = find_record_locked(fi->name);
            bool complete = rec && rec->size == fi->size && rec->mtime == fi->mtime &&
                            rec->chunk_size == EMBROIDERY_CHUNK_SIZE && record_fully_cached_locked(rec);
            xSemaphoreGive(s.mutex);
            if (complete) {
                files_done++;
                app_led_set_sync_progress(files_done, total_files);
            }
        }
        free(snapshot);
        xSemaphoreGive(s.sync_done);
    }
}

/* ---- public API ----------------------------------------------------------- */

/* Shared by app_sd_cache_init() (probe only) and
 * app_sd_cache_format_and_remount() (explicit destructive retry). Always
 * tears down and re-initializes the SPI bus first rather than trying to
 * reason about exactly how much esp_vfs_fat_sdspi_mount() already set up
 * before a partial failure — cheap, and avoids depending on undocumented
 * internal cleanup behavior. */
static esp_err_t do_mount(bool format_if_mount_failed)
{
    spi_bus_free(SD_SPI_HOST); /* no-op / ignored if not previously initialized */

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = CONFIG_EMBROIDERY_SD_MOSI_GPIO,
        .miso_io_num     = CONFIG_EMBROIDERY_SD_MISO_GPIO,
        .sclk_io_num     = CONFIG_EMBROIDERY_SD_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_initialize failed: %d", err);
        s.state = APP_SD_CACHE_ABSENT;
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    slot_cfg.gpio_cs = CONFIG_EMBROIDERY_SD_CS_GPIO;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files             = 4,
        .allocation_unit_size  = 16 * 1024,
    };

    /* The very first attempt at boot can hit the card before its power
     * rail has fully settled — a real, commonly-reported failure mode for
     * "fails at the first SD command (send_if_cond), card works fine
     * elsewhere" that a short retry reliably works around. Cheap insurance
     * either way: harmless if the card was already ready. */
    sdmmc_card_t *card = NULL;
    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed (%d)%s", attempt, max_attempts, err,
                 attempt < max_attempts ? " — retrying" : "");
        if (attempt < max_attempts) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (err != ESP_OK) {
        spi_bus_free(SD_SPI_HOST);
        /* Best-effort split, not verified against real hardware: a card
         * that responds to SPI probing but fails specifically at the FAT
         * mount step comes back as ESP_FAIL; anything else (timeout, no
         * response) means nothing is there to format. */
        s.state = (err == ESP_FAIL) ? APP_SD_CACHE_NEEDS_FORMAT : APP_SD_CACHE_ABSENT;
        ESP_LOGW(TAG, "SD mount failed (%d) — %s", err,
                 s.state == APP_SD_CACHE_NEEDS_FORMAT ? "needs format" : "no card detected");
        return err;
    }

    s.card = card;
    sdmmc_card_print_info(stdout, s.card);
    mkdir(SD_CACHE_DIR, 0775); /* ignore "already exists" */

    if (!s.mutex)       s.mutex       = xSemaphoreCreateMutex();
    if (!s.sync_signal) s.sync_signal = xSemaphoreCreateBinary();
    if (!s.sync_done)   s.sync_done   = xSemaphoreCreateBinary();
    if (!s.records)     s.records     = calloc(EMBROIDERY_MAX_FILES, sizeof(sd_cache_record_t));
    if (!s.mutex || !s.sync_signal || !s.sync_done || !s.records) {
        ESP_LOGE(TAG, "out of memory setting up SD cache");
        return ESP_ERR_NO_MEM;
    }

    /* A freshly (re)mounted card has no index of its own yet; this also
     * correctly discards any previous card's in-RAM index if one was
     * swapped in. */
    s.record_count = 0;
    load_index();

    s.state = APP_SD_CACHE_READY;
    if (!s.sync_task_started) {
        /* Pinned to CPU0, explicitly away from TinyUSB's task (pinned to
         * CPU1 via CONFIG_TINYUSB_TASK_AFFINITY_CPU1). A plain xTaskCreate()
         * has no core affinity at all, so the scheduler is free to land
         * this task's real SD-SPI writes and network fetches on the same
         * core as the USB stack — even though it's lower priority and
         * should in principle be preemptible, sharing a core with
         * time-sensitive USB servicing is exactly the kind of setup that
         * causes intermittent, hard-to-reproduce stalls. */
        s.sync_task_started = (xTaskCreatePinnedToCore(sync_task_fn, "sd_sync", 4096, NULL, 1, NULL, 0) == pdPASS);
        if (!s.sync_task_started) {
            ESP_LOGW(TAG, "failed to start SD sync task — write-through on demand still works");
        }
    }

    ESP_LOGI(TAG, "SD cache ready at %s", SD_CACHE_DIR);
    return ESP_OK;
}

esp_err_t app_sd_cache_init(void)
{
    return do_mount(false);
}

esp_err_t app_sd_cache_format_and_remount(void)
{
    ESP_LOGW(TAG, "formatting SD card (explicit user request)");
    return do_mount(true);
}

app_sd_cache_state_t app_sd_cache_get_state(void)
{
    return s.state;
}

bool app_sd_cache_present(void)
{
    return s.state == APP_SD_CACHE_READY;
}

void app_sd_cache_set_catalog(const proto_file_info_t *files, uint16_t count)
{
    if (s.state != APP_SD_CACHE_READY) return;

    /* Deliberately RAM-only and fast: this always runs before VBUS is
     * reasserted (do_usb_refresh() waits on app_sd_cache_wait_for_sync()
     * right after this call, before touching VBUS at all — see there).
     * Pruning stale blobs and persisting the index/catalog both involve
     * real SD-card I/O and are handled by sync_task_fn() instead, so this
     * function itself stays fast regardless of SD card latency. */
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    free(s.catalog);
    s.catalog = count ? malloc(sizeof(proto_file_info_t) * count) : NULL;
    if (s.catalog) {
        memcpy(s.catalog, files, sizeof(proto_file_info_t) * count);
        s.catalog_count = count;
    } else {
        s.catalog_count = 0;
    }
    s.generation++;
    xSemaphoreGive(s.mutex);

    xSemaphoreTake(s.sync_done, 0);   /* drain any stale completion from a prior generation */
    xSemaphoreGive(s.sync_signal);    /* wake the eager-fill task for the new catalog */
}

/* Blocks until the sync pass for the most recently set_catalog() finishes
 * (or is abandoned for a newer one), or until timeout_ms elapses. Called
 * by do_usb_refresh() before reasserting VBUS, specifically so the whole
 * SD-card + network sync burst for a catalog change happens entirely
 * while the drive is detached — real SD/SPI/network I/O and USB-MSC
 * enumeration never run concurrently, which is what earlier attempts at
 * pacing/prioritizing around that overlap failed to fully prevent. */
bool app_sd_cache_wait_for_sync(uint32_t timeout_ms)
{
    if (s.state != APP_SD_CACHE_READY || !s.sync_task_started) return true;
    bool done = xSemaphoreTake(s.sync_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (done) xSemaphoreGive(s.sync_done); /* leave it set; set_catalog() drains it next time */
    return done;
}
