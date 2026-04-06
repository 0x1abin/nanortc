# nanortc interop tests — libdatachannel as reference peer
#
# Build: cmake -B build -DNANORTC_BUILD_INTEROP_TESTS=ON -DNANORTC_CRYPTO=openssl
#
# SPDX-License-Identifier: MIT

# libdatachannel is C++17; enable CXX for this subdirectory
enable_language(CXX)

include(FetchContent)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.22.5
    GIT_SHALLOW    TRUE
)

# Build with media support (needed for audio/video track interop tests)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

# Disable libsrtp test targets to avoid name collision with nanortc's test_srtp
set(TEST_APPS OFF CACHE BOOL "" FORCE)

# Save and force BUILD_SHARED_LIBS OFF for libdatachannel deps.
# libdatachannel's CMake turns this ON, which would pollute the
# parent nanortc library type on reconfigure.
set(_save_BSL "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

FetchContent_MakeAvailable(libdatachannel)

# Restore
set(BUILD_SHARED_LIBS "${_save_BSL}")

# ---- Common include directories for all interop tests ----
set(INTEROP_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_SOURCE_DIR}/examples/common
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${libdatachannel_SOURCE_DIR}/include
)

# ---- DataChannel interop test ----

add_executable(test_interop_dc
    test_interop_dc.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

target_include_directories(test_interop_dc PRIVATE ${INTEROP_INCLUDE_DIRS})

# Enable C++ linker since libdatachannel is a C++ library
set_target_properties(test_interop_dc PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(test_interop_dc PRIVATE
    nanortc
    unity
    LibDataChannel::LibDataChannelStatic
    pthread
)

# Register with ctest
add_test(NAME interop_datachannel COMMAND test_interop_dc)
set_tests_properties(interop_datachannel PROPERTIES TIMEOUT 120)

# ---- TURN relay interop test (requires network + TURN credentials) ----

add_executable(test_interop_turn
    test_interop_turn.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

target_include_directories(test_interop_turn PRIVATE ${INTEROP_INCLUDE_DIRS})

set_target_properties(test_interop_turn PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(test_interop_turn PRIVATE
    nanortc
    unity
    LibDataChannel::LibDataChannelStatic
    pthread
)

add_test(NAME interop_turn COMMAND test_interop_turn)
set_tests_properties(interop_turn PROPERTIES TIMEOUT 120 LABELS "network")

# ---- Audio interop test (requires NANORTC_FEATURE_AUDIO) ----

if(NANORTC_FEATURE_AUDIO)
    add_executable(test_interop_audio
        test_interop_audio.c
        interop_common.c
        interop_nanortc_media_peer.c
        interop_libdatachannel_media_peer.c
        ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    )

    target_include_directories(test_interop_audio PRIVATE ${INTEROP_INCLUDE_DIRS})
    set_target_properties(test_interop_audio PROPERTIES LINKER_LANGUAGE CXX)

    target_link_libraries(test_interop_audio PRIVATE
        nanortc
        unity
        LibDataChannel::LibDataChannelStatic
        pthread
    )

    add_test(NAME interop_audio COMMAND test_interop_audio)
    set_tests_properties(interop_audio PROPERTIES TIMEOUT 120)
endif()

# ---- Video interop test (requires NANORTC_FEATURE_VIDEO) ----

if(NANORTC_FEATURE_VIDEO)
    add_executable(test_interop_video
        test_interop_video.c
        interop_common.c
        interop_nanortc_media_peer.c
        interop_libdatachannel_media_peer.c
        ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    )

    target_include_directories(test_interop_video PRIVATE ${INTEROP_INCLUDE_DIRS})
    set_target_properties(test_interop_video PROPERTIES LINKER_LANGUAGE CXX)

    target_link_libraries(test_interop_video PRIVATE
        nanortc
        unity
        LibDataChannel::LibDataChannelStatic
        pthread
    )

    add_test(NAME interop_video COMMAND test_interop_video)
    set_tests_properties(interop_video PROPERTIES TIMEOUT 120)
endif()
