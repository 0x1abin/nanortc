idf_component_register(
    SRCS
        "main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_esp.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../common"
    REQUIRES nanortc esp_netif nvs_flash esp_timer esp_http_server protocol_examples_common
    EMBED_TXTFILES
        "index.html"
)
