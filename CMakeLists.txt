cmake_minimum_required(VERSION 3.16)

# ---- ESP-IDF component mode ----
if(DEFINED ENV{IDF_PATH})
    set(NANORTC_CORE_SRCS
        src/nano_rtc.c
        src/nano_log.c
        src/nano_sdp.c
        src/nano_ice.c
        src/nano_stun.c
        src/nano_dtls.c
        src/nano_crc32.c
        crypto/nanortc_crypto_mbedtls.c
    )
    # Feature-conditional sources added via Kconfig (see nanortc_config.h)
    idf_component_register(
        SRCS ${NANORTC_CORE_SRCS}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS src crypto
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

message(STATUS "nanortc features: DC=${NANORTC_FEATURE_DATACHANNEL} DC_REL=${NANORTC_FEATURE_DC_RELIABLE} DC_ORD=${NANORTC_FEATURE_DC_ORDERED} AUDIO=${NANORTC_FEATURE_AUDIO} VIDEO=${NANORTC_FEATURE_VIDEO}")

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
)

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
target_include_directories(nanortc PUBLIC include)
target_include_directories(nanortc PRIVATE src crypto)

# Pass feature flags as compile definitions
target_compile_definitions(nanortc PUBLIC
    NANORTC_FEATURE_DATACHANNEL=$<BOOL:${NANORTC_FEATURE_DATACHANNEL}>
    NANORTC_FEATURE_DC_RELIABLE=$<BOOL:${NANORTC_FEATURE_DC_RELIABLE}>
    NANORTC_FEATURE_DC_ORDERED=$<BOOL:${NANORTC_FEATURE_DC_ORDERED}>
    NANORTC_FEATURE_AUDIO=$<BOOL:${NANORTC_FEATURE_AUDIO}>
    NANORTC_FEATURE_VIDEO=$<BOOL:${NANORTC_FEATURE_VIDEO}>
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
    -Wno-unused-parameter
    -fvisibility=hidden
)

# ---- Sanitizer options ----
option(ADDRESS_SANITIZER "Build with AddressSanitizer" OFF)
if(ADDRESS_SANITIZER)
    target_compile_options(nanortc PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(nanortc PUBLIC -fsanitize=address)
endif()

# ---- Tests ----
option(NANORTC_BUILD_TESTS "Build tests" ON)
if(NANORTC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Examples (Linux host only, not default) ----
option(NANORTC_BUILD_EXAMPLES "Build examples" OFF)
if(NANORTC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- Interop tests (requires network + C++ compiler, not default) ----
option(NANORTC_BUILD_INTEROP_TESTS "Build interop tests against libdatachannel" OFF)
if(NANORTC_BUILD_INTEROP_TESTS)
    add_subdirectory(tests/interop)
endif()
