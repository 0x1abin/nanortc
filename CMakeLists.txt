cmake_minimum_required(VERSION 3.16)

# ---- ESP-IDF component mode ----
if(DEFINED ENV{IDF_PATH})
    set(NANORTC_CORE_SRCS
        src/nano_rtc.c
        src/nano_log.c
        src/nano_sdp.c
        src/nano_ice.c
        src/nano_stun.c
        src/nano_addr.c
        src/nano_dtls.c
        src/nano_crc32.c
        crypto/nanortc_crypto_mbedtls.c
    )

    # TURN relay client (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_TURN)
        list(APPEND NANORTC_CORE_SRCS src/nano_turn.c)
    endif()

    # DataChannel sources (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_DATACHANNEL)
        list(APPEND NANORTC_CORE_SRCS
            src/nano_sctp.c
            src/nano_datachannel.c
            src/nano_crc32c.c
        )
    endif()

    # Media transport (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_AUDIO OR CONFIG_NANORTC_FEATURE_VIDEO)
        list(APPEND NANORTC_CORE_SRCS
            src/nano_rtp.c
            src/nano_rtcp.c
            src/nano_srtp.c
            src/nano_media.c
        )
    endif()
    if(CONFIG_NANORTC_FEATURE_AUDIO)
        list(APPEND NANORTC_CORE_SRCS src/nano_jitter.c)
    endif()
    if(CONFIG_NANORTC_FEATURE_VIDEO)
        list(APPEND NANORTC_CORE_SRCS src/nano_bwe.c src/nano_h264.c src/nano_h265.c)
    endif()

    idf_component_register(
        SRCS ${NANORTC_CORE_SRCS}
        INCLUDE_DIRS include src crypto
        REQUIRES mbedtls
    )
    return()
endif()

# ---- Host build ----
project(nanortc VERSION 0.1.0 LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Feature flags (orthogonal)
option(NANORTC_FEATURE_DATACHANNEL "Enable DataChannel (SCTP+DCEP)" ON)
option(NANORTC_FEATURE_DC_RELIABLE "Enable reliable DataChannel delivery" ON)
option(NANORTC_FEATURE_DC_ORDERED  "Enable ordered DataChannel delivery" ON)
option(NANORTC_FEATURE_AUDIO "Enable audio (RTP/SRTP/Jitter)" OFF)
option(NANORTC_FEATURE_VIDEO "Enable video (RTP/SRTP/BWE)" OFF)
option(NANORTC_FEATURE_IPV6  "Enable IPv6 address support" ON)
option(NANORTC_FEATURE_TURN "Enable TURN relay client (RFC 5766)" ON)

message(STATUS "nanortc features: DC=${NANORTC_FEATURE_DATACHANNEL} DC_REL=${NANORTC_FEATURE_DC_RELIABLE} DC_ORD=${NANORTC_FEATURE_DC_ORDERED} AUDIO=${NANORTC_FEATURE_AUDIO} VIDEO=${NANORTC_FEATURE_VIDEO} IPV6=${NANORTC_FEATURE_IPV6} TURN=${NANORTC_FEATURE_TURN}")

# Crypto backend selection
set(NANORTC_CRYPTO "mbedtls" CACHE STRING "Crypto backend: mbedtls or openssl")
set_property(CACHE NANORTC_CRYPTO PROPERTY STRINGS mbedtls openssl)

message(STATUS "nanortc crypto: ${NANORTC_CRYPTO}")

# User configuration file override (e.g. -DNANORTC_CONFIG_FILE=\"my_config.h\")
set(NANORTC_CONFIG_FILE "" CACHE STRING "Path to user nanortc config header override")

# ---- Core sources (always compiled) ----
set(NANORTC_SOURCES
    src/nano_rtc.c
    src/nano_log.c
    src/nano_sdp.c
    src/nano_ice.c
    src/nano_stun.c
    src/nano_dtls.c
    src/nano_crc32.c
    src/nano_addr.c
)

# ---- TURN relay client ----
if(NANORTC_FEATURE_TURN)
    list(APPEND NANORTC_SOURCES src/nano_turn.c)
endif()

# ---- DataChannel sources ----
if(NANORTC_FEATURE_DATACHANNEL)
    list(APPEND NANORTC_SOURCES
        src/nano_sctp.c
        src/nano_datachannel.c
        src/nano_crc32c.c
    )
endif()

# ---- Media transport sources (shared by audio and video) ----
if(NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO)
    list(APPEND NANORTC_SOURCES
        src/nano_rtp.c
        src/nano_rtcp.c
        src/nano_srtp.c
        src/nano_media.c
    )
endif()

# ---- Audio-specific sources ----
if(NANORTC_FEATURE_AUDIO)
    list(APPEND NANORTC_SOURCES
        src/nano_jitter.c
    )
endif()

# ---- Video-specific sources ----
if(NANORTC_FEATURE_VIDEO)
    list(APPEND NANORTC_SOURCES
        src/nano_bwe.c
        src/nano_h264.c
        src/nano_h265.c
    )
endif()

# ---- Crypto backend ----
if(NANORTC_CRYPTO STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    list(APPEND NANORTC_SOURCES crypto/nanortc_crypto_openssl.c)
else()
    find_package(MbedTLS REQUIRED)
    list(APPEND NANORTC_SOURCES crypto/nanortc_crypto_mbedtls.c)
endif()

# ---- Library target ----
add_library(nanortc ${NANORTC_SOURCES})
target_include_directories(nanortc PUBLIC include src crypto)

# Pass feature flags as compile definitions
target_compile_definitions(nanortc PUBLIC
    NANORTC_FEATURE_DATACHANNEL=$<BOOL:${NANORTC_FEATURE_DATACHANNEL}>
    NANORTC_FEATURE_DC_RELIABLE=$<BOOL:${NANORTC_FEATURE_DC_RELIABLE}>
    NANORTC_FEATURE_DC_ORDERED=$<BOOL:${NANORTC_FEATURE_DC_ORDERED}>
    NANORTC_FEATURE_AUDIO=$<BOOL:${NANORTC_FEATURE_AUDIO}>
    NANORTC_FEATURE_VIDEO=$<BOOL:${NANORTC_FEATURE_VIDEO}>
    NANORTC_FEATURE_IPV6=$<BOOL:${NANORTC_FEATURE_IPV6}>
    NANORTC_FEATURE_TURN=$<BOOL:${NANORTC_FEATURE_TURN}>
)

if(NANORTC_CONFIG_FILE)
    target_compile_definitions(nanortc PUBLIC NANORTC_CONFIG_FILE="${NANORTC_CONFIG_FILE}")
    message(STATUS "nanortc config file: ${NANORTC_CONFIG_FILE}")
endif()

# ---- Link crypto library ----
if(NANORTC_CRYPTO STREQUAL "openssl")
    target_link_libraries(nanortc PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(nanortc PUBLIC NANORTC_CRYPTO_OPENSSL=1)
else()
    target_link_libraries(nanortc PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    target_compile_definitions(nanortc PUBLIC NANORTC_CRYPTO_MBEDTLS=1)
endif()

set_target_properties(nanortc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_compile_options(nanortc PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
)

# ---- Sanitizer options ----
option(ADDRESS_SANITIZER "Build with AddressSanitizer" OFF)
if(ADDRESS_SANITIZER)
    target_compile_options(nanortc PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(nanortc PUBLIC -fsanitize=address)
endif()

# ---- Code coverage (gcov/lcov) ----
option(NANORTC_COVERAGE "Build with code coverage instrumentation" OFF)
if(NANORTC_COVERAGE)
    target_compile_options(nanortc PUBLIC --coverage -fprofile-arcs -ftest-coverage)
    target_link_options(nanortc PUBLIC --coverage)
endif()

# ---- Tests ----
option(NANORTC_BUILD_TESTS "Build tests" ON)
if(NANORTC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Fuzz testing (requires Clang with libFuzzer) ----
option(NANORTC_BUILD_FUZZ "Build fuzz testing harnesses (requires clang)" OFF)
if(NANORTC_BUILD_FUZZ)
    add_subdirectory(tests/fuzz)
endif()

# ---- Examples (Linux host only, not default) ----
option(NANORTC_BUILD_EXAMPLES "Build examples" OFF)
if(NANORTC_BUILD_EXAMPLES)
    # Profile level for gating examples: 0=core, 1=DC, 2=media
    set(PROFILE_NUM 0)
    if(NANORTC_FEATURE_DATACHANNEL)
        set(PROFILE_NUM 1)
    endif()
    if(NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO)
        set(PROFILE_NUM 2)
    endif()
    add_subdirectory(examples)
endif()

# ---- Interop tests (requires network + C++ compiler, not default) ----
option(NANORTC_BUILD_INTEROP_TESTS "Build interop tests against libdatachannel" OFF)
if(NANORTC_BUILD_INTEROP_TESTS)
    add_subdirectory(tests/interop)
endif()
