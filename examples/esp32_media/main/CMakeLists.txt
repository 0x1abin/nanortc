set(SAMPLE_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sample_data")
set(PACK_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/pack_frames.py")

set(SRCS
    "main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/run_loop_session.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/webserver_esp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../common/media_source_embed.c"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "${CMAKE_CURRENT_SOURCE_DIR}/../../common"
    REQUIRES nanortc esp_wifi esp_netif nvs_flash esp_timer esp_http_server protocol_examples_common
    EMBED_TXTFILES "index.html"
)

# Auto-pack media blobs from sample frames at build time. Only the enabled
# tracks are packed, so a video-only / audio-only build saves flash and keeps
# the corresponding extern in main.c resolved.
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(python PYTHON)

if(CONFIG_EXAMPLE_ENABLE_VIDEO)
    set(VIDEO_BLOB "${build_dir}/video.blob")
    add_custom_command(
        OUTPUT "${VIDEO_BLOB}"
        COMMAND "${python}" "${PACK_TOOL}"
                "${SAMPLE_DATA_DIR}/h264SampleFrames" 0 "${VIDEO_BLOB}"
        DEPENDS "${PACK_TOOL}"
        COMMENT "Packing H.264 frames -> video.blob"
        VERBATIM
    )
    target_add_binary_data(${COMPONENT_LIB} "${VIDEO_BLOB}" BINARY)
endif()

if(CONFIG_EXAMPLE_ENABLE_AUDIO)
    set(AUDIO_BLOB "${build_dir}/audio.blob")
    add_custom_command(
        OUTPUT "${AUDIO_BLOB}"
        COMMAND "${python}" "${PACK_TOOL}"
                "${SAMPLE_DATA_DIR}/opusSampleFrames" 0 "${AUDIO_BLOB}"
        DEPENDS "${PACK_TOOL}"
        COMMENT "Packing Opus frames -> audio.blob"
        VERBATIM
    )
    target_add_binary_data(${COMPONENT_LIB} "${AUDIO_BLOB}" BINARY)
endif()
