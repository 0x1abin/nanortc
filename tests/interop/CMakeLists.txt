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

# Minimal build: data channels only, no websocket/media/examples/tests
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)

# Save and force BUILD_SHARED_LIBS OFF for libdatachannel deps.
# libdatachannel's CMake turns this ON, which would pollute the
# parent nanortc library type on reconfigure.
set(_save_BSL "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)

FetchContent_MakeAvailable(libdatachannel)

# Restore
set(BUILD_SHARED_LIBS "${_save_BSL}")

# --- Interop test executable ---

add_executable(test_interop_dc
    test_interop_dc.c
    interop_common.c
    interop_nanortc_peer.c
    interop_libdc_peer.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
)

target_include_directories(test_interop_dc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_SOURCE_DIR}/examples/common
    ${CMAKE_SOURCE_DIR}/tests
    ${libdatachannel_SOURCE_DIR}/include
)

# Enable C++ linker since libdatachannel is a C++ library
set_target_properties(test_interop_dc PROPERTIES LINKER_LANGUAGE CXX)

# nanortc internal sctp_init() collides with usrsctp's sctp_init().
# Both are independent implementations; allow the linker to pick the
# first definition (nanortc is listed first, so its symbol wins for
# nanortc code, and usrsctp's C++ TU already has its own copy).
target_link_options(test_interop_dc PRIVATE
    "LINKER:--allow-multiple-definition"
)

target_link_libraries(test_interop_dc PRIVATE
    nanortc
    LibDataChannel::LibDataChannelStatic
    pthread
)

# Register with ctest
add_test(NAME interop_datachannel COMMAND test_interop_dc)
set_tests_properties(interop_datachannel PROPERTIES TIMEOUT 30)
