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

function(add_interop_test target test_name timeout labels)
    add_executable(${target} ${ARGN})
    target_include_directories(${target} PRIVATE ${INTEROP_INCLUDE_DIRS})
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(${target} PRIVATE
        nanortc
        unity
        LibDataChannel::LibDataChannelStatic
        pthread
    )
    add_test(NAME ${test_name} COMMAND ${target})
    if(labels)
        set_tests_properties(${test_name} PROPERTIES TIMEOUT ${timeout} LABELS "${labels}")
        if(labels MATCHES "(^|;)network($|;)")
            set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    else()
        set_tests_properties(${test_name} PROPERTIES TIMEOUT ${timeout})
    endif()
endfunction()

# ---- DataChannel interop test ----

add_interop_test(test_interop_dc interop_datachannel 120 ""
    test_interop_dc.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

# ---- TURN relay interop test (requires network + TURN credentials) ----

add_interop_test(test_interop_turn interop_turn 120 "network"
    test_interop_turn.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

# ---- TURN relay-only interop test ----
# Forces the libdatachannel side into RTC_TRANSPORT_POLICY_RELAY so all
# traffic must traverse the TURN server. Skipped at runtime if no TURN
# server is reachable (default 127.0.0.1:3478, override with NANORTC_TURN_*).

add_interop_test(test_interop_turn_relay interop_turn_relay 180 "network;turn-relay"
    test_interop_turn_relay.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

# ---- TURN relay-only interop test, nanortc side ----
# Mirror image of test_interop_turn_relay: nanortc is the TURN client this
# time. Both peers advertise only relay candidates, avoiding assumptions about
# the runner's host/NAT topology. Exercises the client-side relay fixes
# (via_turn signal, lazy wrap, permission fan-out, stats counters). Skipped if
# no suitable external TURN topology is reachable.

add_interop_test(test_interop_turn_relay_nanortc interop_turn_relay_nanortc 180 "network;turn-relay"
    test_interop_turn_relay_nanortc.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

# ---- srflx interop test ----
# Local fake STUN responder + libdatachannel host, with nanortc forced into
# srflx-only mode (no host candidate). Verifies that srflx is actually used
# for the connectivity check (selected_local_type == SRFLX) and DataChannel
# data flows over the srflx-selected pair. No external network required.

add_interop_test(test_interop_srflx interop_srflx 60 ""
    test_interop_srflx.c
    interop_common.c
    interop_fake_stun.c
    interop_nanortc_peer.c
    interop_libdatachannel_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    ${CMAKE_SOURCE_DIR}/examples/common/ice_server_resolve.c
)

# ---- Audio interop test (requires NANORTC_FEATURE_AUDIO) ----

if(NANORTC_FEATURE_AUDIO)
    add_interop_test(test_interop_audio interop_audio 120 ""
        test_interop_audio.c
        interop_common.c
        interop_nanortc_media_peer.c
        interop_libdatachannel_media_peer.c
        ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    )
endif()

# ---- Video interop test (requires NANORTC_FEATURE_VIDEO) ----

if(NANORTC_FEATURE_VIDEO)
    add_interop_test(test_interop_video interop_video 120 ""
        test_interop_video.c
        interop_common.c
        interop_nanortc_media_peer.c
        interop_libdatachannel_media_peer.c
        ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
    )
endif()
