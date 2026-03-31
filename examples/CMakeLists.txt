# nanortc examples — Linux host only (not for ESP-IDF)

add_subdirectory(linux_datachannel)
add_subdirectory(browser_interop)

if(PROFILE_NUM GREATER_EQUAL 2)
    add_subdirectory(linux_media_send)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS QUIET opus)
endif()
if(OPUS_FOUND)
    add_subdirectory(tools)
else()
    message(STATUS "libopus not found — skipping opus tools")
endif()
