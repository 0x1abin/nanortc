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
set(VIDEO_TESTS test_h264 test_bwe)

# Tests that require DataChannel
set(DC_TESTS test_datachannel)

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
