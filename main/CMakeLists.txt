set(srcs
    "app_backend_client.c"
    "app_button.c"
    "app_button_watch.c"
    "app_cache.c"
    "app_discovery.c"
    "app_main.c"
    "app_provision.c"
    "app_virtual_fat.c"
    "app_wifi.c"
)

# Exactly one status backend is compiled in — both implement app_led.h.
# The GEEK has no onboard RGB LED (GPIO38 is the SD card's CS line), so
# it flood-fills its LCD with the status color instead; see Kconfig
# EMBROIDERY_STATUS_BACKEND_*.
if(CONFIG_EMBROIDERY_STATUS_BACKEND_LCD)
    list(APPEND srcs "app_status_lcd.c")
else()
    list(APPEND srcs "app_led.c")
endif()

if(CONFIG_EMBROIDERY_SD_CACHE)
    list(APPEND srcs "app_sd_cache.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    EMBED_FILES "hartje_1.PES" "provision_form.html"
    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_lcd driver fatfs sdmmc esp_driver_sdspi
)
