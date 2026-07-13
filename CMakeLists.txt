cmake_minimum_required(VERSION 3.16)

set(NANORTC_BASE_SRCS
    src/nano_rtc.c
    src/nano_rtc_media.c
    src/nano_rtc_negotiate.c
    src/nano_log.c
    src/nano_sdp.c
    src/nano_ice.c
    src/nano_stun.c
    src/nano_addr.c
    src/nano_dtls.c
    src/nano_crc32.c
)
set(NANORTC_TURN_SRCS src/nano_turn.c)
set(NANORTC_DATACHANNEL_SRCS
    src/nano_sctp.c
    src/nano_datachannel.c
    src/nano_crc32c.c
)
set(NANORTC_MEDIA_SRCS
    src/nano_rtp.c
    src/nano_rtcp.c
    src/nano_srtp.c
    src/nano_media.c
)
set(NANORTC_AUDIO_SRCS src/nano_jitter.c)
set(NANORTC_VIDEO_SRCS
    src/nano_bwe.c
    src/nano_twcc.c
    src/nano_rate_control.c
    src/nano_annex_b.c
    src/nano_h264.c
    src/nano_reorder.c
    src/nano_fec.c
)
set(NANORTC_H265_SRCS
    src/nano_h265.c
    src/nano_base64.c
)

# ---- ESP-IDF component mode ----
# ESP_PLATFORM is set by ESP-IDF's component build. IDF_PATH only identifies
# an installed SDK and may legitimately be exported during an ordinary host
# build, so it must not select component mode by itself.
if(ESP_PLATFORM)
    set(NANORTC_CORE_SRCS ${NANORTC_BASE_SRCS} crypto/nanortc_crypto_mbedtls.c)

    # TURN relay client (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_TURN)
        list(APPEND NANORTC_CORE_SRCS ${NANORTC_TURN_SRCS})
    endif()

    # DataChannel sources (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_DATACHANNEL)
        list(APPEND NANORTC_CORE_SRCS ${NANORTC_DATACHANNEL_SRCS})
    endif()

    # Media transport (conditional on Kconfig)
    if(CONFIG_NANORTC_FEATURE_AUDIO OR CONFIG_NANORTC_FEATURE_VIDEO)
        list(APPEND NANORTC_CORE_SRCS ${NANORTC_MEDIA_SRCS})
    endif()
    if(CONFIG_NANORTC_FEATURE_AUDIO)
        list(APPEND NANORTC_CORE_SRCS ${NANORTC_AUDIO_SRCS})
    endif()
    if(CONFIG_NANORTC_FEATURE_VIDEO)
        list(APPEND NANORTC_CORE_SRCS ${NANORTC_VIDEO_SRCS})
        if(CONFIG_NANORTC_FEATURE_H265)
            list(APPEND NANORTC_CORE_SRCS ${NANORTC_H265_SRCS})
        endif()
    endif()

    idf_component_register(
        SRCS ${NANORTC_CORE_SRCS}
        INCLUDE_DIRS include src crypto
        REQUIRES mbedtls
    )

    set_target_properties(${COMPONENT_LIB} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror
        -fvisibility=hidden
    )

    # Optional profile header override (measurement / tuning hook).
    # When NANORTC_PROFILE_HDR is set in the environment, prepend its
    # contents to every nanortc translation unit so that the #ifndef
    # guards in nanortc_config.h honour the profile's overrides.
    # Passing `-include` and the path as separate arguments lets CMake
    # quote the path — safe against spaces / shell metacharacters.
    if(DEFINED ENV{NANORTC_PROFILE_HDR})
        set(_nanortc_profile_hdr "$ENV{NANORTC_PROFILE_HDR}")
        target_compile_options(${COMPONENT_LIB} PRIVATE
            -include "${_nanortc_profile_hdr}")
    endif()

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
option(NANORTC_FEATURE_H265  "Enable H.265/HEVC video codec (RFC 7798)" OFF)
option(NANORTC_FEATURE_IPV6  "Enable IPv6 address support" ON)
option(NANORTC_FEATURE_TURN "Enable TURN relay client (RFC 5766)" ON)
option(NANORTC_FEATURE_ICE_SRFLX "Enable ICE server-reflexive candidate gathering (STUN srflx)" ON)
option(NANORTC_FEATURE_VIDEO_RATE_CONTROL "Enable adaptive video capability-ladder control" OFF)
option(NANORTC_FEATURE_VIDEO_REORDER "Enable receive-side video packet reordering" OFF)
option(NANORTC_FEATURE_VIDEO_NACK_RX "Enable receiver-generated RTCP NACK" OFF)
option(NANORTC_FEATURE_VIDEO_FEC "Enable ULPFEC video protection" OFF)
option(NANORTC_FEATURE_VIDEO_PACING "Enable send-side video pacing" ON)
option(NANORTC_FEATURE_VIDEO_AUTO_PLI "Enable automatic PLI on receive loss" ON)
option(NANORTC_FEC_ADAPTIVE "Adapt FEC group size to measured loss" ON)
set(NANORTC_TX_SLOT_COUNT "4" CACHE STRING "Owned transient transmit slot count (1,2,4,8,16,32)")
set_property(CACHE NANORTC_TX_SLOT_COUNT PROPERTY STRINGS 1 2 4 8 16 32)

if(NOT NANORTC_TX_SLOT_COUNT MATCHES "^(1|2|4|8|16|32)$")
    message(FATAL_ERROR "NANORTC_TX_SLOT_COUNT must be one of: 1, 2, 4, 8, 16, 32")
endif()

if(NANORTC_FEATURE_H265 AND NOT NANORTC_FEATURE_VIDEO)
    message(FATAL_ERROR "NANORTC_FEATURE_H265 requires NANORTC_FEATURE_VIDEO=ON")
endif()
if(NANORTC_FEATURE_VIDEO_RATE_CONTROL AND NOT NANORTC_FEATURE_VIDEO)
    message(FATAL_ERROR "NANORTC_FEATURE_VIDEO_RATE_CONTROL requires NANORTC_FEATURE_VIDEO=ON")
endif()
if(NANORTC_FEATURE_VIDEO_REORDER AND NOT NANORTC_FEATURE_VIDEO)
    message(FATAL_ERROR "NANORTC_FEATURE_VIDEO_REORDER requires NANORTC_FEATURE_VIDEO=ON")
endif()
if(NANORTC_FEATURE_VIDEO_NACK_RX AND NOT NANORTC_FEATURE_VIDEO)
    message(FATAL_ERROR "NANORTC_FEATURE_VIDEO_NACK_RX requires NANORTC_FEATURE_VIDEO=ON")
endif()
if(NANORTC_FEATURE_VIDEO_FEC AND NOT NANORTC_FEATURE_VIDEO)
    message(FATAL_ERROR "NANORTC_FEATURE_VIDEO_FEC requires NANORTC_FEATURE_VIDEO=ON")
endif()
if(NANORTC_FEATURE_VIDEO_FEC AND NOT NANORTC_FEATURE_VIDEO_REORDER)
    message(FATAL_ERROR "NANORTC_FEATURE_VIDEO_FEC requires NANORTC_FEATURE_VIDEO_REORDER=ON")
endif()

message(STATUS "nanortc features: DC=${NANORTC_FEATURE_DATACHANNEL} DC_REL=${NANORTC_FEATURE_DC_RELIABLE} DC_ORD=${NANORTC_FEATURE_DC_ORDERED} AUDIO=${NANORTC_FEATURE_AUDIO} VIDEO=${NANORTC_FEATURE_VIDEO} H265=${NANORTC_FEATURE_H265} IPV6=${NANORTC_FEATURE_IPV6} TURN=${NANORTC_FEATURE_TURN} ICE_SRFLX=${NANORTC_FEATURE_ICE_SRFLX} RATE_CONTROL=${NANORTC_FEATURE_VIDEO_RATE_CONTROL} REORDER=${NANORTC_FEATURE_VIDEO_REORDER} NACK_RX=${NANORTC_FEATURE_VIDEO_NACK_RX} FEC=${NANORTC_FEATURE_VIDEO_FEC} PACING=${NANORTC_FEATURE_VIDEO_PACING} AUTO_PLI=${NANORTC_FEATURE_VIDEO_AUTO_PLI} FEC_ADAPTIVE=${NANORTC_FEC_ADAPTIVE}")

# Crypto backend selection
set(NANORTC_CRYPTO "mbedtls" CACHE STRING "Crypto backend: mbedtls or openssl")
set_property(CACHE NANORTC_CRYPTO PROPERTY STRINGS mbedtls openssl)
set(_nanortc_crypto_backends mbedtls openssl)
if(NOT NANORTC_CRYPTO IN_LIST _nanortc_crypto_backends)
    message(FATAL_ERROR
        "Unsupported NANORTC_CRYPTO='${NANORTC_CRYPTO}'; expected one of: mbedtls, openssl")
endif()

message(STATUS "nanortc crypto: ${NANORTC_CRYPTO}")

# User configuration file override (e.g. -DNANORTC_CONFIG_FILE=\"my_config.h\")
set(NANORTC_CONFIG_FILE "" CACHE STRING "Path to user nanortc config header override")

# ---- Core sources (always compiled) ----
set(NANORTC_SOURCES ${NANORTC_BASE_SRCS})

# ---- TURN relay client ----
if(NANORTC_FEATURE_TURN)
    list(APPEND NANORTC_SOURCES ${NANORTC_TURN_SRCS})
endif()

# ---- DataChannel sources ----
if(NANORTC_FEATURE_DATACHANNEL)
    list(APPEND NANORTC_SOURCES ${NANORTC_DATACHANNEL_SRCS})
endif()

# ---- Media transport sources (shared by audio and video) ----
if(NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO)
    list(APPEND NANORTC_SOURCES ${NANORTC_MEDIA_SRCS})
endif()

# ---- Audio-specific sources ----
if(NANORTC_FEATURE_AUDIO)
    list(APPEND NANORTC_SOURCES ${NANORTC_AUDIO_SRCS})
endif()

# ---- Video-specific sources ----
if(NANORTC_FEATURE_VIDEO)
    list(APPEND NANORTC_SOURCES ${NANORTC_VIDEO_SRCS})
    if(NANORTC_FEATURE_H265)
        list(APPEND NANORTC_SOURCES ${NANORTC_H265_SRCS})
    endif()
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
    NANORTC_FEATURE_H265=$<BOOL:${NANORTC_FEATURE_H265}>
    NANORTC_FEATURE_IPV6=$<BOOL:${NANORTC_FEATURE_IPV6}>
    NANORTC_FEATURE_TURN=$<BOOL:${NANORTC_FEATURE_TURN}>
    NANORTC_FEATURE_ICE_SRFLX=$<BOOL:${NANORTC_FEATURE_ICE_SRFLX}>
    NANORTC_FEATURE_VIDEO_RATE_CONTROL=$<BOOL:${NANORTC_FEATURE_VIDEO_RATE_CONTROL}>
    NANORTC_FEATURE_VIDEO_REORDER=$<BOOL:${NANORTC_FEATURE_VIDEO_REORDER}>
    NANORTC_FEATURE_VIDEO_NACK_RX=$<BOOL:${NANORTC_FEATURE_VIDEO_NACK_RX}>
    NANORTC_FEATURE_VIDEO_FEC=$<BOOL:${NANORTC_FEATURE_VIDEO_FEC}>
    NANORTC_FEATURE_VIDEO_PACING=$<BOOL:${NANORTC_FEATURE_VIDEO_PACING}>
    NANORTC_FEATURE_VIDEO_AUTO_PLI=$<BOOL:${NANORTC_FEATURE_VIDEO_AUTO_PLI}>
    NANORTC_FEC_ADAPTIVE=$<BOOL:${NANORTC_FEC_ADAPTIVE}>
)

if(NANORTC_CONFIG_FILE)
    target_compile_definitions(nanortc PUBLIC NANORTC_CONFIG_FILE="${NANORTC_CONFIG_FILE}")
    message(STATUS "nanortc config file: ${NANORTC_CONFIG_FILE}")
else()
    target_compile_definitions(nanortc PUBLIC NANORTC_TX_SLOT_COUNT=${NANORTC_TX_SLOT_COUNT})
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
    add_subdirectory(examples)
endif()

# ---- Interop tests (requires network + C++ compiler, not default) ----
option(NANORTC_BUILD_INTEROP_TESTS "Build interop tests against libdatachannel" OFF)
if(NANORTC_BUILD_INTEROP_TESTS)
    add_subdirectory(tests/interop)
endif()
