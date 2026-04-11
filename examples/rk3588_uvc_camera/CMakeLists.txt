# rk3588_uvc_camera — V4L2 USB camera → H.264 encoder → browser via nanortc
#
# Linux only. Supports two capture/encode backends:
#   - FFmpeg (default): h264_rkmpp hardware encoder via libav*
#   - GStreamer:        mpph264enc via gstreamer-rockchip
#
# Select backend: -DRK3588_CAPTURE_BACKEND=ffmpeg|gstreamer
# Default: auto-detect (prefer FFmpeg, fallback to GStreamer)

if(NOT (UNIX AND NOT APPLE))
    message(STATUS "rk3588_uvc_camera: skipping (Linux-only)")
    return()
endif()

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(STATUS "rk3588_uvc_camera: pkg-config not found, skipping")
    return()
endif()

# Detect available backends
pkg_check_modules(FFAV QUIET IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libavdevice)
pkg_check_modules(GST QUIET IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# Select backend (auto or explicit)
if(NOT DEFINED RK3588_CAPTURE_BACKEND)
    if(FFAV_FOUND)
        set(RK3588_CAPTURE_BACKEND "ffmpeg")
    elseif(GST_FOUND)
        set(RK3588_CAPTURE_BACKEND "gstreamer")
    else()
        message(STATUS "rk3588_uvc_camera: neither FFmpeg nor GStreamer found, skipping")
        return()
    endif()
endif()

if(RK3588_CAPTURE_BACKEND STREQUAL "ffmpeg")
    if(NOT FFAV_FOUND)
        message(FATAL_ERROR "rk3588_uvc_camera: FFmpeg backend requested but libav* not found")
    endif()
    set(CAPTURE_SRC capture_ffmpeg.c)
    set(CAPTURE_LIB PkgConfig::FFAV)
    set(CAPTURE_DEF RK3588_CAPTURE_FFMPEG)
elseif(RK3588_CAPTURE_BACKEND STREQUAL "gstreamer")
    if(NOT GST_FOUND)
        message(FATAL_ERROR "rk3588_uvc_camera: GStreamer backend requested but not found")
    endif()
    set(CAPTURE_SRC capture_gstreamer.c)
    set(CAPTURE_LIB PkgConfig::GST)
    set(CAPTURE_DEF RK3588_CAPTURE_GSTREAMER)
else()
    message(FATAL_ERROR "rk3588_uvc_camera: unknown backend '${RK3588_CAPTURE_BACKEND}'")
endif()

message(STATUS "rk3588_uvc_camera: backend=${RK3588_CAPTURE_BACKEND}")

set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_executable(rk3588_uvc_camera
    main.c
    frame_queue.c
    sig_queue.c
    session.c
    ${CAPTURE_SRC}
    ${COMMON}/run_loop_linux.c
    ${COMMON}/http_signaling.c
    ${COMMON}/sig_discovery.c
)

target_compile_definitions(rk3588_uvc_camera PRIVATE ${CAPTURE_DEF})

target_include_directories(rk3588_uvc_camera PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${COMMON}
)

target_link_libraries(rk3588_uvc_camera PRIVATE
    nanortc
    ${CAPTURE_LIB}
)

# HD/4K IDR fragments overflow the default queue — bump to 512 globally.
# This must apply to both the library and the example so struct layouts
# stay in sync, hence PUBLIC on the nanortc target.
target_compile_definitions(nanortc PUBLIC
    NANORTC_CONFIG_FILE="nanortc_app_config.h"
)
target_include_directories(nanortc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
