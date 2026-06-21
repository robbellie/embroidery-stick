/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "tinyusb.h"
#include "soc/gpio_sig_map.h"
#include "sdkconfig.h"

#include "app_wifi.h"
#include "app_backend_client.h"
#include "app_cache.h"
#include "app_virtual_fat.h"
#include "app_led.h"
#include "app_button.h"
#include "app_provision.h"
#include "app_discovery.h"
#include "embroidery_protocol.h"

static const char *TAG = "main";

#define LOGICAL_DISK_NUM    1
#define VERSION_POLL_MS     5000

/* ---- USB attach/detach ------------------------------------------------- */

static void usbd_vbus_enable(bool enable)
{
    esp_rom_gpio_connect_in_signal(
        enable ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT,
        USB_OTG_VBUSVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(
        enable ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT,
        USB_SRP_BVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(
        enable ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT,
        USB_SRP_SESSEND_IN_IDX, 1);
}

/* ---- USB activity tracking (for idle detection) ------------------------ */

static volatile uint32_t s_last_usb_read_ms = 0;

static bool usb_is_idle(void)
{
    return (xTaskGetTickCount() * portTICK_PERIOD_MS - s_last_usb_read_ms) > 3000;
}

/* ---- Disk refresh ------------------------------------------------------ */

static void do_usb_refresh(proto_file_info_t *files, uint16_t count)
{
    ESP_LOGI(TAG, "USB refresh: detaching");
    usbd_vbus_enable(false);
    /* Long enough that host USB stacks/file managers reliably treat this
     * as a real disconnect (not a debounced glitch) and actually re-read
     * the directory on reattach — 50ms was too short for some hosts to
     * notice at all, leaving the file manager showing a stale listing
     * until the user manually replugged/refreshed. */
    vTaskDelay(pdMS_TO_TICKS(500));

    cache_invalidate_all();
    vfat_init(files, count, CONFIG_EMBROIDERY_VOLUME_LABEL);

    ESP_LOGI(TAG, "USB refresh: reattaching");
    usbd_vbus_enable(true);
}

/* ---- Backend host resolution (auto-discovery) --------------------------- */

#ifndef CONFIG_EMBROIDERY_STUB_MODE
/* Tracks the backend currently in use, so version_poll_task can detect
 * when a fresh discovery finds a *different* backend than the one
 * configured at boot (e.g. the stick was plugged in before the backend
 * laptop was even running). Manual NVS/Kconfig host override disables
 * rediscovery entirely. */
static char     s_backend_host[128];
static uint16_t s_backend_port;
static bool     s_backend_manual;

/* No-op if the backend host was manually configured. Otherwise broadcasts
 * for the backend; if a different host responds than the one currently
 * in use, reinitializes the backend connection to it. Returns true if a
 * (re)connect should be retried immediately. */
static bool try_rediscover_backend(void)
{
    if (s_backend_manual) return false;

    char host[64];
    uint16_t port;
    if (app_discovery_find_backend(host, sizeof(host), &port) != ESP_OK) {
        return false;
    }
    if (strcmp(host, s_backend_host) == 0 && port == s_backend_port) {
        return false; /* same backend already in use, nothing to do */
    }

    ESP_LOGI(TAG, "rediscovered backend at %s:%u (was %s:%u)",
             host, port, s_backend_host, s_backend_port);
    backend_deinit();
    strlcpy(s_backend_host, host, sizeof(s_backend_host));
    s_backend_port = port;
    backend_init(s_backend_host, s_backend_port);
    return true;
}
#endif

/* ---- Version polling task ---------------------------------------------- */

#ifndef CONFIG_EMBROIDERY_STUB_MODE
static void version_poll_task(void *arg)
{
    /* Retry until we successfully read the initial disk version.
     * If the first call fails and we leave known_version=0, the next
     * successful poll will see a spurious "version changed" and trigger
     * an unnecessary USB refresh mid-read. */
    uint64_t known_version = 0;
    while (backend_get_version(&known_version) != ESP_OK) {
        if (try_rediscover_backend()) continue; /* retry immediately against new host */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    app_led_set_state(APP_LED_STATE_CONNECTED);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(VERSION_POLL_MS));

        if (!app_wifi_is_connected()) continue;

        uint64_t new_version = 0;
        if (backend_get_version(&new_version) != ESP_OK) {
            app_led_set_state(APP_LED_STATE_DISCOVERING);
            try_rediscover_backend();
            continue;
        }
        app_led_set_state(APP_LED_STATE_CONNECTED);
        if (new_version == known_version) continue;

        ESP_LOGI(TAG, "backend version changed %llu → %llu, scheduling refresh",
                 (unsigned long long)known_version, (unsigned long long)new_version);

        /* Wait until USB is idle */
        while (!usb_is_idle()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        proto_file_info_t *files = NULL;
        uint16_t count = 0;
        if (backend_list_files(&files, &count) == ESP_OK) {
            do_usb_refresh(files, count);
            free(files);
            known_version = new_version;
        }
    }
}
#endif

/* ---- NVS config helpers ------------------------------------------------ */

static void nvs_read_or_default(const char *key, char *buf, size_t size, const char *fallback)
{
    nvs_handle_t h;
    if (nvs_open("memory", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = size;
        if (nvs_get_str(h, key, buf, &sz) != ESP_OK) strlcpy(buf, fallback, size);
        nvs_close(h);
    } else {
        strlcpy(buf, fallback, size);
    }
}

/* ---- Ejection state ---------------------------------------------------- */
static bool s_ejected[LOGICAL_DISK_NUM] = {true};

#ifndef CONFIG_EMBROIDERY_STUB_MODE
/* Clears any manually-pinned backend host/port, re-enabling auto-discovery.
 * Called when the user explicitly holds the button to re-provision —
 * "start fresh" should reset backend pinning too, not just WiFi, or a
 * stale manual override from a previous network setup would silently
 * keep auto-discovery disabled forever. */
static void clear_backend_override(void)
{
    nvs_handle_t h;
    if (nvs_open("memory", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "backhost");
        nvs_erase_key(h, "backport");
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "cleared backend host/port override — will auto-discover");
    }
}
#endif

/* ======================================================================== */
/* app_main                                                                  */
/* ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "Embroidery Stick booting");

    /* Log memory at startup */
    ESP_LOGI(TAG, "Internal RAM free: %u KB, PSRAM free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024));

    ESP_ERROR_CHECK(app_led_init());
    ESP_ERROR_CHECK(app_button_init());

#ifdef CONFIG_EMBROIDERY_STUB_MODE
    /* ---- Stub mode: init disk before USB starts (no VBUS toggle needed) */
    ESP_LOGW(TAG, "STUB MODE enabled");

    proto_file_info_t *files = NULL;
    uint16_t count = 0;
    ESP_ERROR_CHECK(backend_stub_init(&files, &count));
    ESP_ERROR_CHECK(cache_init(backend_stub_fetch, EMBROIDERY_CHUNK_SIZE,
                               CONFIG_EMBROIDERY_CACHE_SLOTS));
    vfat_init(files, count, CONFIG_EMBROIDERY_VOLUME_LABEL);
    free(files);

    tinyusb_config_t tusb_cfg = {0};
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC started (%u files)", count);

#else
    /* ---- Platform init shared by both the STA and provisioning paths -- */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* esp_wifi_init() (and the STA netif) must happen exactly once per
     * boot, shared by both the normal STA path and the provisioning path
     * — calling it twice (e.g. STA connect times out, falls back into
     * provisioning) panics the device. */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    /* ---- Decide: provisioning (no creds, or button held) vs normal boot */
    bool button_held = app_button_is_held(CONFIG_EMBROIDERY_BUTTON_HOLD_MS);
    if (button_held || !app_wifi_has_credentials()) {
        ESP_LOGI(TAG, "%s — entering WiFi provisioning mode",
                 button_held ? "button held at boot" : "no stored WiFi credentials");
        if (button_held) {
            clear_backend_override();
        }
        app_led_set_state(APP_LED_STATE_PROVISIONING);
        app_provision_start();
        /* unreachable: app_provision_start() ends in esp_restart() */
    }

    /* ---- Normal mode: connect to WiFi first, USB MSC comes up only after */
    app_led_set_state(APP_LED_STATE_CONNECTING);
    ESP_ERROR_CHECK(app_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi...");
    if (app_wifi_wait_connected_timeout(CONFIG_EMBROIDERY_STA_CONNECT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "STA connect timed out — falling back to provisioning");
        app_led_set_state(APP_LED_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1500));
        app_led_set_state(APP_LED_STATE_PROVISIONING);
        app_provision_start();
        /* unreachable */
    }

    /* ---- WiFi is up — resolve the backend (manual NVS override, else
     * broadcast discovery) before connecting to it. Broadcast discovery
     * needs a real IP, so this must happen after WiFi connects, not
     * before. */
    app_led_set_state(APP_LED_STATE_DISCOVERING);

    char backend_host[128];
    char backend_port_str[8];
    nvs_read_or_default("backhost", backend_host,     sizeof(backend_host),     CONFIG_EMBROIDERY_BACKEND_HOST);
    nvs_read_or_default("backport", backend_port_str, sizeof(backend_port_str), "7892");
    uint16_t backend_port = (uint16_t)atoi(backend_port_str);
    if (backend_port == 0) backend_port = EMBROIDERY_DEFAULT_PORT;

    s_backend_manual = (strlen(backend_host) > 0);
    if (!s_backend_manual) {
        ESP_LOGI(TAG, "no backend host configured — broadcasting discovery");
        char discovered_host[64];
        uint16_t discovered_port;
        if (app_discovery_find_backend(discovered_host, sizeof(discovered_host), &discovered_port) == ESP_OK) {
            strlcpy(backend_host, discovered_host, sizeof(backend_host));
            backend_port = discovered_port;
        } else {
            ESP_LOGW(TAG, "backend discovery timed out — will keep retrying in background");
        }
    }
    strlcpy(s_backend_host, backend_host, sizeof(s_backend_host));
    s_backend_port = backend_port;

    ESP_ERROR_CHECK(backend_init(backend_host, backend_port));
    ESP_ERROR_CHECK(cache_init(backend_read_file, EMBROIDERY_CHUNK_SIZE,
                               CONFIG_EMBROIDERY_CACHE_SLOTS));

    /* ---- USB MSC comes up only now that WiFi is confirmed connected --- */
    vfat_init(NULL, 0, CONFIG_EMBROIDERY_VOLUME_LABEL);
    tinyusb_config_t tusb_cfg = {0};
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB MSC started (empty disk)");

    proto_file_info_t *files = NULL;
    uint16_t count = 0;
    if (backend_list_files(&files, &count) == ESP_OK) {
        do_usb_refresh(files, count);
        free(files);
        app_led_set_state(APP_LED_STATE_CONNECTED);
    } else {
        ESP_LOGW(TAG, "Backend unreachable — presenting empty disk");
    }

    /* Start version polling in background */
    xTaskCreate(version_poll_task, "ver_poll", 4096, NULL, 3, NULL);
#endif
}

/* ======================================================================== */
/* TinyUSB MSC callbacks                                                     */
/* ======================================================================== */

void tud_mount_cb(void)
{
    for (uint8_t i = 0; i < LOGICAL_DISK_NUM; i++) s_ejected[i] = false;
    ESP_LOGI("usb", "mounted");
}

void tud_umount_cb(void)
{
    ESP_LOGW("usb", "unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                         uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "ESP",          3);
    memcpy(product_id,  "EmbroideryStk", 13);
    memcpy(product_rev, "1.0",          3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (lun >= LOGICAL_DISK_NUM || s_ejected[lun]) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = vfat_get_sector_count();
    *block_size  = vfat_get_sector_size();
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;   /* read-only */
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (lun >= LOGICAL_DISK_NUM) return false;
    if (load_eject && !start) {
        s_ejected[lun] = true;
        ESP_LOGI("usb", "ejected");
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           void *buffer, uint32_t bufsize)
{
    (void)offset;
    if (lun >= LOGICAL_DISK_NUM) return -1;

    s_last_usb_read_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    vfat_read_result_t r = vfat_read_sectors(lba, (uint8_t *)buffer, bufsize);
    if (r == VFAT_READ_OK) return (int32_t)bufsize;

    if (r == VFAT_READ_PENDING) {
        /* Cache miss — fetch queued in background. Return BUSY so TinyUSB
         * retries the same LBA internally without reporting an error to the host. */
        return 0;
    }
    return -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                            uint8_t *buffer, uint32_t bufsize)
{
    /* Disk is read-only — pretend success to keep host happy */
    (void)lun; (void)lba; (void)offset; (void)buffer;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                          void *buffer, uint16_t bufsize)
{
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; }
