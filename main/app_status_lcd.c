/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Status backend for boards with an ST7789 LCD instead of an RGB LED
 * (e.g. the Waveshare ESP32-S3-GEEK, whose GPIO38 is the SD card's CS
 * line, not a status LED). Implements the same app_led.h interface as
 * app_led.c. The screen is split into two independently-redrawn regions:
 *   - a top color band, flood-filled/blinked the same way app_led.c
 *     drives a WS2812 pixel — glanceable state at a distance;
 *   - a text area below it showing extra context (WiFi SSID, IP) via
 *     app_led_set_info(), redrawn only when that context changes so it
 *     doesn't flicker while the band blinks.
 * Only one of app_led.c / this file is compiled in, selected by
 * CONFIG_EMBROIDERY_STATUS_BACKEND_* (see main/CMakeLists.txt).
 *
 * Panel geometry (gap offsets, RGB vs BGR element order, color inversion)
 * is unverified against real hardware as of writing — this was built
 * ahead of the board arriving. See the Kconfig help text for
 * EMBROIDERY_LCD_X_GAP/Y_GAP.
 */
#include "app_led.h"
#include "font8x8_basic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "status_lcd";

#define LCD_H_RES   CONFIG_EMBROIDERY_LCD_H_RES
#define LCD_V_RES   CONFIG_EMBROIDERY_LCD_V_RES
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define BAND_H      48                        /* top color band, px */
#define TEXT_H      (LCD_V_RES - BAND_H)       /* text area, px */
#define TEXT_SCALE  2                          /* 16x16 px per glyph */
#define TEXT_LINE_H (8 * TEXT_SCALE + 4)

/* Desk indicator, not a flashlight — keep it dim. */
/* Unlike a WS2812 LED (where dimming the drive current is how you control
 * visible brightness), an LCD's overall brightness is the backlight's job
 * — a dim pixel *value* here just means a weak, easily-misread color
 * signal (confirmed on hardware: solid green at the WS2812-tuned dim
 * value of 30 read as yellow). Full saturation for clarity; dim the
 * backlight instead if it's too bright. */
#define LED_BRIGHTNESS 255

typedef struct {
    uint8_t r, g, b;
    uint32_t on_ms;   /* 0 = solid on */
    uint32_t off_ms;  /* ignored when on_ms == 0 */
} led_pattern_t;

static const led_pattern_t s_patterns[] = {
    [APP_LED_STATE_OFF]          = { 0,               0,               0,               0,    0   },
    [APP_LED_STATE_PROVISIONING] = { 0,               0,               LED_BRIGHTNESS,  500,  500 },
    [APP_LED_STATE_CONNECTING]   = { LED_BRIGHTNESS,  LED_BRIGHTNESS,  0,               125,  125 },
    [APP_LED_STATE_DISCOVERING]  = { 0,               LED_BRIGHTNESS,  LED_BRIGHTNESS,  300,  300 },
    [APP_LED_STATE_CONNECTED]    = { 0,               LED_BRIGHTNESS,  0,               0,    0   },
    [APP_LED_STATE_ERROR]        = { LED_BRIGHTNESS,  0,               0,               0,    0   },
    [APP_LED_STATE_OFFLINE]      = { LED_BRIGHTNESS,  0,               LED_BRIGHTNESS,  0,    0   },
};

static struct {
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t panel_mutex; /* guards concurrent draw_bitmap calls */
    SemaphoreHandle_t xfer_done;   /* signaled when the in-flight SPI transfer completes */
    uint16_t *band_buf;            /* LCD_H_RES * BAND_H pixels, RGB565 */
    uint16_t *text_buf;            /* LCD_H_RES * TEXT_H pixels, RGB565 */
    volatile app_led_state_t state;
    char line1[24];
    char line2[24];
    char line3[24]; /* SD cache hit/miss stats — see app_led_set_stats() */
    char line4[24]; /* SD cache sync progress — see app_led_set_sync_progress() */
} s;

/* esp_lcd_panel_draw_bitmap() over SPI is asynchronous — it can queue the
 * transfer and return before the hardware has actually finished reading
 * the buffer (see esp_lcd_panel_io_spi.c: the completion callback only
 * fires on the last of possibly several chunked sub-transactions). Without
 * waiting for that here, a fast state change could overwrite band_buf/
 * text_buf while the previous color's transfer was still mid-flight,
 * producing a genuinely torn/blended frame — confirmed on real hardware
 * as a solid "yellow" where a state transition from amber (CONNECTING) to
 * green (CONNECTED) should have landed cleanly on green. */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s.xfer_done, &woken);
    return woken == pdTRUE;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void draw_char(uint16_t *buf, int buf_w, int x0, int y0, char ch, uint16_t fg, int scale)
{
    if ((unsigned char)ch > 0x7E) {
        ch = '?';
    }
    const uint8_t *glyph = font8x8_basic[(unsigned char)ch];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = y0 + row * scale + sy;
                if (py < 0 || py >= TEXT_H) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x0 + col * scale + sx;
                    if (px < 0 || px >= buf_w) continue;
                    buf[py * buf_w + px] = fg;
                }
            }
        }
    }
}

/* Draws left-to-right, silently clipping once it runs off the right edge —
 * safe regardless of how long str is (e.g. a 32-char WiFi SSID). */
static void draw_text(uint16_t *buf, int buf_w, int x0, int y0, const char *str, uint16_t fg, int scale)
{
    int x = x0;
    for (const char *p = str; *p && x + 8 * scale <= buf_w; p++, x += 8 * scale + 1) {
        draw_char(buf, buf_w, x, y0, *p, fg, scale);
    }
}

static void panel_draw(int y_start, int y_end, const uint16_t *buf)
{
    xSemaphoreTake(s.panel_mutex, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s.panel, 0, y_start, LCD_H_RES, y_end, (void *)buf);
    /* Block until this transfer actually completes before releasing the
     * mutex, so the caller (or the next one in line) can safely mutate
     * this same buffer again the moment panel_draw() returns. */
    xSemaphoreTake(s.xfer_done, portMAX_DELAY);
    xSemaphoreGive(s.panel_mutex);
}

static void redraw_text_area(void)
{
    memset(s.text_buf, 0, (size_t)LCD_H_RES * TEXT_H * sizeof(uint16_t));
    uint16_t white = rgb565(255, 255, 255);
    if (s.line1[0]) {
        draw_text(s.text_buf, LCD_H_RES, 2, 6, s.line1, white, TEXT_SCALE);
    }
    if (s.line2[0]) {
        draw_text(s.text_buf, LCD_H_RES, 2, 6 + TEXT_LINE_H, s.line2, white, TEXT_SCALE);
    }
    if (s.line3[0]) {
        draw_text(s.text_buf, LCD_H_RES, 2, 6 + 2 * TEXT_LINE_H, s.line3, white, TEXT_SCALE);
    }
    if (s.line4[0]) {
        draw_text(s.text_buf, LCD_H_RES, 2, 6 + 3 * TEXT_LINE_H, s.line4, white, TEXT_SCALE);
    }
    panel_draw(BAND_H, LCD_V_RES, s.text_buf);
}

static void fill_band(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color = rgb565(r, g, b);
    size_t px_count = (size_t)LCD_H_RES * BAND_H;
    for (size_t i = 0; i < px_count; i++) {
        s.band_buf[i] = color;
    }
    panel_draw(0, BAND_H, s.band_buf);
}

static void led_task(void *arg)
{
    bool on = true;
    while (1) {
        const led_pattern_t *p = &s_patterns[s.state];

        if (p->on_ms == 0) {
            /* Solid (or off) */
            fill_band(p->r, p->g, p->b);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (on) {
            fill_band(p->r, p->g, p->b);
        } else {
            fill_band(0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(on ? p->on_ms : p->off_ms));
        on = !on;
    }
}

esp_err_t app_led_init(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_EMBROIDERY_LCD_BL_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&bl_cfg);
    if (err != ESP_OK) return err;
#ifdef CONFIG_EMBROIDERY_LCD_BL_ACTIVE_LOW
    gpio_set_level(CONFIG_EMBROIDERY_LCD_BL_GPIO, 0); /* backlight on */
#else
    gpio_set_level(CONFIG_EMBROIDERY_LCD_BL_GPIO, 1);
#endif

    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = CONFIG_EMBROIDERY_LCD_CLK_GPIO,
        .mosi_io_num     = CONFIG_EMBROIDERY_LCD_MOSI_GPIO,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = (size_t)LCD_H_RES * (BAND_H > TEXT_H ? BAND_H : TEXT_H) * sizeof(uint16_t),
    };
    err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %d", err);
        return err;
    }

    s.xfer_done = xSemaphoreCreateBinary();
    if (!s.xfer_done) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num    = CONFIG_EMBROIDERY_LCD_CS_GPIO,
        .dc_gpio_num    = CONFIG_EMBROIDERY_LCD_DC_GPIO,
        .spi_mode       = 0,
        .pclk_hz        = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits   = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_trans_done,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %d", err);
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_EMBROIDERY_LCD_RST_GPIO,
#ifdef CONFIG_EMBROIDERY_LCD_BGR
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s.panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %d", err);
        return err;
    }

    esp_lcd_panel_reset(s.panel);
    esp_lcd_panel_init(s.panel);
#ifdef CONFIG_EMBROIDERY_LCD_SWAP_XY
    esp_lcd_panel_swap_xy(s.panel, true);
#endif
    esp_lcd_panel_mirror(s.panel,
#ifdef CONFIG_EMBROIDERY_LCD_MIRROR_X
        true,
#else
        false,
#endif
#ifdef CONFIG_EMBROIDERY_LCD_MIRROR_Y
        true
#else
        false
#endif
    );
    esp_lcd_panel_set_gap(s.panel, CONFIG_EMBROIDERY_LCD_X_GAP, CONFIG_EMBROIDERY_LCD_Y_GAP);
    esp_lcd_panel_disp_on_off(s.panel, true);

    s.panel_mutex = xSemaphoreCreateMutex();
    if (!s.panel_mutex) return ESP_ERR_NO_MEM;

    size_t band_size = (size_t)LCD_H_RES * BAND_H * sizeof(uint16_t);
    size_t text_size = (size_t)LCD_H_RES * TEXT_H * sizeof(uint16_t);
    s.band_buf = heap_caps_malloc(band_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s.band_buf) s.band_buf = heap_caps_malloc(band_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s.text_buf = heap_caps_malloc(text_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s.text_buf) s.text_buf = heap_caps_malloc(text_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s.band_buf || !s.text_buf) {
        ESP_LOGE(TAG, "failed to allocate LCD framebuffers");
        return ESP_ERR_NO_MEM;
    }

    s.state = APP_LED_STATE_OFF;
    fill_band(0, 0, 0);
    redraw_text_area();

#ifdef CONFIG_EMBROIDERY_LCD_COLOR_SELFTEST
    /* Temporary calibration aid — see Kconfig help. Deliberately no text
     * and no partial regions (both caused confounds in an earlier version
     * of this test — a torn transition and an oversized-looking text
     * area were mistaken for color bugs): every pixel on the whole panel
     * is set to the exact same value at once, holding well past any
     * possible transition artifact, so whatever is seen is unambiguously
     * "this RGB input maps to this displayed color" and nothing else. */
    static const struct { const char *name; uint8_t r, g, b; } selftest[] = {
        { "RED",   255, 0,   0   },
        { "GREEN", 0,   255, 0   },
        { "BLUE",  0,   0,   255 },
        { "WHITE", 255, 255, 255 },
        { "BLACK", 0,   0,   0   },
    };
    for (size_t i = 0; i < sizeof(selftest) / sizeof(selftest[0]); i++) {
        ESP_LOGI(TAG, "color self-test: %s", selftest[i].name);
        uint16_t c = rgb565(selftest[i].r, selftest[i].g, selftest[i].b);
        for (size_t j = 0; j < (size_t)LCD_H_RES * BAND_H; j++) s.band_buf[j] = c;
        panel_draw(0, BAND_H, s.band_buf);
        for (size_t j = 0; j < (size_t)LCD_H_RES * TEXT_H; j++) s.text_buf[j] = c;
        panel_draw(BAND_H, LCD_V_RES, s.text_buf);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
    fill_band(0, 0, 0);
    redraw_text_area();
#endif

    if (xTaskCreate(led_task, "status_lcd", 2560, NULL, 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_led_set_state(app_led_state_t state)
{
    s.state = state;
}

void app_led_set_info(const char *line1, const char *line2)
{
    strlcpy(s.line1, line1 ? line1 : "", sizeof(s.line1));
    strlcpy(s.line2, line2 ? line2 : "", sizeof(s.line2));
    redraw_text_area();
}

void app_led_set_stats(uint32_t sd_hits, uint32_t net_fetches)
{
    snprintf(s.line3, sizeof(s.line3), "SD:%lu NET:%lu", (unsigned long)sd_hits, (unsigned long)net_fetches);
    redraw_text_area();
}

void app_led_set_sync_progress(uint16_t files_done, uint16_t files_total)
{
    if (files_total == 0) {
        s.line4[0] = '\0';
    } else if (files_done >= files_total) {
        snprintf(s.line4, sizeof(s.line4), "Synced %u/%u", (unsigned)files_done, (unsigned)files_total);
    } else {
        snprintf(s.line4, sizeof(s.line4), "Syncing %u/%u", (unsigned)files_done, (unsigned)files_total);
    }
    redraw_text_area();
}
