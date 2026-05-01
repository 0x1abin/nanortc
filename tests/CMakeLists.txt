# nanortc test targets
# Each test_*.c file becomes its own executable
# Uses Unity test framework (vendored in third_party/unity/)

# ---- Unity library ----
add_library(unity STATIC ${CMAKE_SOURCE_DIR}/third_party/unity/src/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_SOURCE_DIR}/third_party/unity/src)
set_target_properties(unity PROPERTIES C_STANDARD 99)

# ---- Test targets ----
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")

# Tests that require media transport (AUDIO or VIDEO enabled)
set(MEDIA_TESTS test_rtp test_srtp test_rtcp test_media)

# Tests that require AUDIO specifically
set(AUDIO_TESTS test_jitter)

# Tests that require VIDEO specifically
set(VIDEO_TESTS test_h264 test_bwe test_twcc test_annex_b)

# Tests that require the H.265 sub-feature of VIDEO
set(H265_TESTS test_h265 test_base64)

# Tests that require DataChannel
set(DC_TESTS test_datachannel test_crc32)

# Tests that require TURN
set(TURN_TESTS test_turn)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)

    # Skip media-transport tests when neither AUDIO nor VIDEO is enabled
    list(FIND MEDIA_TESTS ${test_name} _is_media_test)
    if(_is_media_test GREATER -1 AND NOT NANORTC_FEATURE_AUDIO AND NOT NANORTC_FEATURE_VIDEO)
        continue()
    endif()

    # Skip AUDIO-only tests when AUDIO is not enabled
    list(FIND AUDIO_TESTS ${test_name} _is_audio_test)
    if(_is_audio_test GREATER -1 AND NOT NANORTC_FEATURE_AUDIO)
        continue()
    endif()

    # Skip VIDEO-only tests when VIDEO is not enabled
    list(FIND VIDEO_TESTS ${test_name} _is_video_test)
    if(_is_video_test GREATER -1 AND NOT NANORTC_FEATURE_VIDEO)
        continue()
    endif()

    # Skip H.265 sub-feature tests when H265 is not enabled
    list(FIND H265_TESTS ${test_name} _is_h265_test)
    if(_is_h265_test GREATER -1 AND NOT NANORTC_FEATURE_H265)
        continue()
    endif()

    # Skip DC-only tests when DATACHANNEL is not enabled
    list(FIND DC_TESTS ${test_name} _is_dc_test)
    if(_is_dc_test GREATER -1 AND NOT NANORTC_FEATURE_DATACHANNEL)
        continue()
    endif()

    # Skip TURN tests when TURN is not enabled
    list(FIND TURN_TESTS ${test_name} _is_turn_test)
    if(_is_turn_test GREATER -1 AND NOT NANORTC_FEATURE_TURN)
        continue()
    endif()

    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE nanortc unity)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---- Output-lifetime: small-ring variant ----
# PR-2 §Verification: parametrize the lifetime burst tests to
# NANORTC_OUT_QUEUE_SIZE=4 + NANORTC_VIDEO_PKT_RING_SIZE=4 so the aliasing
# window is small enough for the existing tests (drain-before-wrap,
# overrun-aliases-pre-drain, lazy-wrap-rewrites, oversized-payload-drops)
# to actually trip in CI. At default sizes (32/32) the burst the test
# generates fits without wrap, masking regressions.
#
# The library's struct layout is sized at compile time, so we cannot just
# pass `-D` on the test target alone — the test would link against a
# library compiled with default sizes and read garbage. Instead we compile
# NANORTC_SOURCES directly into the test executable with the overrides;
# ccache makes the rebuild near-free.
if(NANORTC_FEATURE_VIDEO AND NANORTC_FEATURE_TURN)
    # NANORTC_SOURCES is defined relative to the project root in the parent
    # CMakeLists.txt. Rebase to absolute paths since tests/ is a subdir.
    set(_min_lifetime_sources "")
    foreach(_src ${NANORTC_SOURCES})
        list(APPEND _min_lifetime_sources "${CMAKE_SOURCE_DIR}/${_src}")
    endforeach()
    add_executable(test_output_lifetime_min
        ${CMAKE_CURRENT_SOURCE_DIR}/test_output_lifetime.c
        ${_min_lifetime_sources}
    )
    target_link_libraries(test_output_lifetime_min PRIVATE unity)
    target_include_directories(test_output_lifetime_min PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
    )
    target_compile_definitions(test_output_lifetime_min PRIVATE
        NANORTC_FEATURE_DATACHANNEL=$<BOOL:${NANORTC_FEATURE_DATACHANNEL}>
        NANORTC_FEATURE_DC_RELIABLE=$<BOOL:${NANORTC_FEATURE_DC_RELIABLE}>
        NANORTC_FEATURE_DC_ORDERED=$<BOOL:${NANORTC_FEATURE_DC_ORDERED}>
        NANORTC_FEATURE_AUDIO=$<BOOL:${NANORTC_FEATURE_AUDIO}>
        NANORTC_FEATURE_VIDEO=$<BOOL:${NANORTC_FEATURE_VIDEO}>
        NANORTC_FEATURE_H265=$<BOOL:${NANORTC_FEATURE_H265}>
        NANORTC_FEATURE_IPV6=$<BOOL:${NANORTC_FEATURE_IPV6}>
        NANORTC_FEATURE_TURN=$<BOOL:${NANORTC_FEATURE_TURN}>
        NANORTC_OUT_QUEUE_SIZE=4
        NANORTC_VIDEO_PKT_RING_SIZE=4
    )
    if(NANORTC_CRYPTO STREQUAL "openssl")
        target_link_libraries(test_output_lifetime_min PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(test_output_lifetime_min PRIVATE NANORTC_CRYPTO_OPENSSL=1)
    else()
        target_link_libraries(test_output_lifetime_min PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
        target_compile_definitions(test_output_lifetime_min PRIVATE NANORTC_CRYPTO_MBEDTLS=1)
    endif()
    set_target_properties(test_output_lifetime_min PROPERTIES C_STANDARD 99)
    add_test(NAME test_output_lifetime_min COMMAND test_output_lifetime_min)
endif()
