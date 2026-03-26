# FindMbedTLS.cmake - Find mbedTLS libraries and headers
#
# Supports mbedtls 2.x, 3.x, and 4.x.
# mbedtls 4.0 renamed libmbedcrypto → libtfpsacrypto and provides
# a CMake config package. We try CONFIG mode first, then fall back
# to manual search.
#
# Imported targets:
#   MbedTLS::mbedtls    - SSL/TLS library
#   MbedTLS::mbedx509   - X.509 library
#   MbedTLS::mbedcrypto - Crypto library (alias for tfpsacrypto on 4.x)
#
# Result variables:
#   MbedTLS_FOUND       - True if mbedTLS was found
#   MBEDTLS_INCLUDE_DIR - Include directory
#   MBEDTLS_LIBRARIES   - All mbedTLS libraries

# ---- Try CMake config package first (mbedtls 4.x provides one) ----
find_package(MbedTLS CONFIG QUIET)

if(MbedTLS_FOUND)
    # mbedtls 4.x config provides MbedTLS::tfpsacrypto instead of MbedTLS::mbedcrypto.
    # Create a compatibility wrapper so downstream code can use MbedTLS::mbedcrypto.
    # (ALIAS of IMPORTED requires CMake 3.18+; INTERFACE works on 3.16+)
    if(TARGET MbedTLS::tfpsacrypto AND NOT TARGET MbedTLS::mbedcrypto)
        add_library(MbedTLS::mbedcrypto INTERFACE IMPORTED)
        target_link_libraries(MbedTLS::mbedcrypto INTERFACE MbedTLS::tfpsacrypto)
    endif()
    return()
endif()

# ---- Manual search (mbedtls 2.x/3.x) ----

find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)

find_library(MBEDTLS_LIBRARY mbedtls)
find_library(MBEDX509_LIBRARY mbedx509)
# Try tfpsacrypto first (4.x), then mbedcrypto (2.x/3.x)
find_library(MBEDCRYPTO_LIBRARY NAMES tfpsacrypto mbedcrypto)

# Detect mbedtls version from build_info.h (4.x) or version.h (2.x/3.x)
if(MBEDTLS_INCLUDE_DIR)
    if(EXISTS "${MBEDTLS_INCLUDE_DIR}/mbedtls/build_info.h")
        file(STRINGS "${MBEDTLS_INCLUDE_DIR}/mbedtls/build_info.h" _MBEDTLS_VERSION_LINE
             REGEX "^#define MBEDTLS_VERSION_STRING ")
    endif()
    if(NOT _MBEDTLS_VERSION_LINE AND EXISTS "${MBEDTLS_INCLUDE_DIR}/mbedtls/version.h")
        file(STRINGS "${MBEDTLS_INCLUDE_DIR}/mbedtls/version.h" _MBEDTLS_VERSION_LINE
             REGEX "^#define MBEDTLS_VERSION_STRING ")
    endif()
    if(_MBEDTLS_VERSION_LINE)
        string(REGEX REPLACE ".*\"(.*)\".*" "\\1" MbedTLS_VERSION "${_MBEDTLS_VERSION_LINE}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MbedTLS
    REQUIRED_VARS MBEDTLS_INCLUDE_DIR MBEDTLS_LIBRARY MBEDX509_LIBRARY MBEDCRYPTO_LIBRARY
    VERSION_VAR MbedTLS_VERSION
)

if(MbedTLS_FOUND)
    set(MBEDTLS_LIBRARIES ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})

    if(NOT TARGET MbedTLS::mbedcrypto)
        add_library(MbedTLS::mbedcrypto UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedcrypto PROPERTIES
            IMPORTED_LOCATION "${MBEDCRYPTO_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
        )
    endif()

    if(NOT TARGET MbedTLS::mbedx509)
        add_library(MbedTLS::mbedx509 UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedx509 PROPERTIES
            IMPORTED_LOCATION "${MBEDX509_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES MbedTLS::mbedcrypto
        )
    endif()

    if(NOT TARGET MbedTLS::mbedtls)
        add_library(MbedTLS::mbedtls UNKNOWN IMPORTED)
        set_target_properties(MbedTLS::mbedtls PROPERTIES
            IMPORTED_LOCATION "${MBEDTLS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MBEDTLS_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "MbedTLS::mbedx509;MbedTLS::mbedcrypto"
        )
    endif()
endif()

mark_as_advanced(MBEDTLS_INCLUDE_DIR MBEDTLS_LIBRARY MBEDX509_LIBRARY MBEDCRYPTO_LIBRARY)
