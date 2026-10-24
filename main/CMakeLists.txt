idf_component_register(
    SRCS
        "main.cc"
        "ir/ir_encoder.cc"
        "ir/ir_db.cc"
        "app/remote_state.cc"
        "app/macro_engine.cc"
        "app/wifi_ap.cc"
        "app/web_server.cc"
    INCLUDE_DIRS "."
    REQUIRES driver esp_http_server nvs_flash esp_wifi esp_netif esp_event
    EMBED_FILES "web/index.html"
)
