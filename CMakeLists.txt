cmake_minimum_required(VERSION 3.16)

# ---- ESP-IDF component mode ----
if(DEFINED ENV{IDF_PATH})
    set(NANORTC_CORE_SRCS
        src/nano_rtc.c
        src/nano_sdp.c
        src/nano_ice.c
        src/nano_stun.c
        src/nano_dtls.c
        src/nano_sctp.c
        src/nano_datachannel.c
        src/nano_crc32c.c
        src/nano_crc32.c
        crypto/nano_crypto_mbedtls.c
    )
    # Audio/Media sources added via NANORTC_PROFILE in Kconfig
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

# Profile selection
set(NANORTC_PROFILE "DATA" CACHE STRING "Build profile: DATA, AUDIO, or MEDIA")
set_property(CACHE NANORTC_PROFILE PROPERTY STRINGS DATA AUDIO MEDIA)

if(NANORTC_PROFILE STREQUAL "AUDIO")
    set(PROFILE_NUM 2)
elseif(NANORTC_PROFILE STREQUAL "MEDIA")
    set(PROFILE_NUM 3)
else()
    set(PROFILE_NUM 1)
endif()

message(STATUS "nanortc profile: ${NANORTC_PROFILE} (${PROFILE_NUM})")

# Crypto backend selection
set(NANORTC_CRYPTO "mbedtls" CACHE STRING "Crypto backend: mbedtls or openssl")
set_property(CACHE NANORTC_CRYPTO PROPERTY STRINGS mbedtls openssl)

message(STATUS "nanortc crypto: ${NANORTC_CRYPTO}")

# ---- Core sources (all profiles) ----
set(NANORTC_SOURCES
    src/nano_rtc.c
    src/nano_sdp.c
    src/nano_ice.c
    src/nano_stun.c
    src/nano_dtls.c
    src/nano_sctp.c
    src/nano_datachannel.c
    src/nano_crc32c.c
    src/nano_crc32.c
)

# ---- Audio sources (AUDIO and MEDIA profiles) ----
if(PROFILE_NUM GREATER_EQUAL 2)
    list(APPEND NANORTC_SOURCES
        src/nano_rtp.c
        src/nano_rtcp.c
        src/nano_srtp.c
        src/nano_jitter.c
    )
endif()

# ---- Media sources (MEDIA profile only) ----
if(PROFILE_NUM GREATER_EQUAL 3)
    list(APPEND NANORTC_SOURCES
        src/nano_bwe.c
    )
endif()

# ---- Crypto backend ----
if(NANORTC_CRYPTO STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    list(APPEND NANORTC_SOURCES crypto/nano_crypto_openssl.c)
else()
    find_package(MbedTLS REQUIRED)
    list(APPEND NANORTC_SOURCES crypto/nano_crypto_mbedtls.c)
endif()

# ---- Library target ----
add_library(nanortc ${NANORTC_SOURCES})
target_include_directories(nanortc PUBLIC include)
target_include_directories(nanortc PRIVATE src crypto)
target_compile_definitions(nanortc PUBLIC NANORTC_PROFILE=${PROFILE_NUM})

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
