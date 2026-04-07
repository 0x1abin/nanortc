set(SRCS
    "main.c"
    "camera.c"
    "encoder.c"
    "microphone.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/webserver_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/ice_server_resolve.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../../common"
    EMBED_TXTFILES "index.html"
)
