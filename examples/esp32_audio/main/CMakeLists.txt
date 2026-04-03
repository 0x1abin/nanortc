set(SRCS
    "main.c"
    "music_gen.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/webserver_esp.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../../common"
    REQUIRES nanortc esp_wifi esp_netif nvs_flash esp_timer esp_http_server esp_audio_codec protocol_examples_common
    EMBED_TXTFILES "index.html"
)
