# nanortc examples — Linux host only (not for ESP-IDF)

add_subdirectory(browser_interop)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS QUIET opus)
endif()
if(OPUS_FOUND)
    add_subdirectory(tools)
else()
    message(STATUS "libopus not found — skipping opus tools")
endif()

if(APPLE)
    add_subdirectory(macos_camera)
endif()
