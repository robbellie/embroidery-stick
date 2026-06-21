idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    EMBED_FILES "hartje_1.PES" "provision_form.html"
    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash
)
