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

if(UNIX AND NOT APPLE)
    # One host camera example covering every encoder backend (libx264 /
    # NVENC / RK3588 mpp), selected at runtime with -e. It injects its own
    # NANORTC_CONFIG_FILE onto the shared nanortc target, so build it in its
    # own build dir if you combine it with another config-injecting example.
    add_subdirectory(linux_uvc_camera)
endif()
