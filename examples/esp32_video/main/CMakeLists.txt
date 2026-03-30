set(SRCS
    "main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/media_source_file.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../../common"
    REQUIRES nanortc esp_wifi esp_netif nvs_flash esp_timer esp_http_server protocol_examples_common
    PRIV_REQUIRES esp_driver_sdmmc fatfs vfs
    EMBED_TXTFILES "index.html"
)
