# linux_uvc_camera — V4L2 USB camera → browser via nanortc
#                     with BWE-driven adaptive bitrate (+ optional ALSA → Opus)
#
# Linux only. One binary, runtime-selectable encoder (-e):
#   - libx264     software H.264 (universal default, no GPU needed)
#   - hevc_nvenc  NVIDIA GPU H.265  (build the library with -DNANORTC_FEATURE_H265=ON)
#   - h264_nvenc  NVIDIA GPU H.264
#   - h264_rkmpp  Rockchip MPP hardware H.264 (RK3399/RK356x/RK3576/RK3588)
#
# Capture/encode backend:
#   - FFmpeg (default): libav*, resolves the -e encoder by name
#   - GStreamer:        mpph264enc / mpph265enc on Rockchip MPP (-DCAPTURE_BACKEND=gstreamer)
# Default: auto-detect (prefer FFmpeg, fall back to GStreamer).
#
# H.265 / NVENC build (own build dir; enables H.265 in the library):
#   cmake -B build-nvenc -DNANORTC_CRYPTO=openssl -DNANORTC_BUILD_EXAMPLES=ON \
#         -DNANORTC_FEATURE_VIDEO=ON -DNANORTC_FEATURE_H265=ON
#   cmake --build build-nvenc --target linux_uvc_camera
#   ./build-nvenc/examples/linux_uvc_camera/linux_uvc_camera -e hevc_nvenc

if(NOT (UNIX AND NOT APPLE))
    message(STATUS "linux_uvc_camera: skipping (Linux-only)")
    return()
endif()

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(STATUS "linux_uvc_camera: pkg-config not found, skipping")
    return()
endif()

# Detect available video backends.
pkg_check_modules(FFAV QUIET IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libavdevice)
pkg_check_modules(GST QUIET IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

# Detect audio dependencies (libopus + libasound). Audio is an optional
# add-on: video builds with or without it.
pkg_check_modules(OPUS QUIET IMPORTED_TARGET opus)
pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)

if(OPUS_FOUND AND ALSA_FOUND AND NANORTC_FEATURE_AUDIO)
    set(UVC_HAS_AUDIO TRUE)
    message(STATUS "linux_uvc_camera: audio=on (libopus ${OPUS_VERSION}, libasound ${ALSA_VERSION})")
elseif(OPUS_FOUND AND ALSA_FOUND AND NOT NANORTC_FEATURE_AUDIO)
    set(UVC_HAS_AUDIO FALSE)
    message(WARNING "linux_uvc_camera: libopus and libasound found but NANORTC_FEATURE_AUDIO=OFF. "
                    "Reconfigure with -DNANORTC_FEATURE_AUDIO=ON to enable microphone audio.")
else()
    set(UVC_HAS_AUDIO FALSE)
    message(STATUS "linux_uvc_camera: audio=off (libopus=${OPUS_FOUND} libasound=${ALSA_FOUND})")
endif()

# Select capture backend (auto or explicit).
if(NOT DEFINED CAPTURE_BACKEND)
    if(FFAV_FOUND)
        set(CAPTURE_BACKEND "ffmpeg")
    elseif(GST_FOUND)
        set(CAPTURE_BACKEND "gstreamer")
    else()
        message(STATUS "linux_uvc_camera: neither FFmpeg nor GStreamer found, skipping")
        return()
    endif()
endif()

if(CAPTURE_BACKEND STREQUAL "ffmpeg")
    if(NOT FFAV_FOUND)
        message(FATAL_ERROR "linux_uvc_camera: FFmpeg backend requested but libav* not found")
    endif()
    set(CAPTURE_SRC capture_ffmpeg.c)
    set(CAPTURE_LIB PkgConfig::FFAV)
    # FFmpeg is the default backend; the source selects the encoder by name at
    # runtime, so no compile define is needed here.
elseif(CAPTURE_BACKEND STREQUAL "gstreamer")
    if(NOT GST_FOUND)
        message(FATAL_ERROR "linux_uvc_camera: GStreamer backend requested but not found")
    endif()
    set(CAPTURE_SRC capture_gstreamer.c)
    set(CAPTURE_LIB PkgConfig::GST)
    set(CAPTURE_DEF LINUX_UVC_CAPTURE_GSTREAMER)
else()
    message(FATAL_ERROR "linux_uvc_camera: unknown backend '${CAPTURE_BACKEND}'")
endif()

message(STATUS "linux_uvc_camera: backend=${CAPTURE_BACKEND}, H265=${NANORTC_FEATURE_H265}")

set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_executable(linux_uvc_camera
    main.c
    media_queue.c
    media_pipeline.c
    sig_queue.c
    ${CAPTURE_SRC}
    ${COMMON}/run_loop_linux.c
    ${COMMON}/http_signaling.c
    ${COMMON}/sig_discovery.c
    ${COMMON}/multi_session.c
    ${COMMON}/bwe_coordinator.c
)

if(CAPTURE_DEF)
    target_compile_definitions(linux_uvc_camera PRIVATE ${CAPTURE_DEF})
endif()

target_include_directories(linux_uvc_camera PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${COMMON}
)

target_link_libraries(linux_uvc_camera PRIVATE
    nanortc
    ${CAPTURE_LIB}
)

if(UVC_HAS_AUDIO)
    target_sources(linux_uvc_camera PRIVATE
        audio_capture_alsa.c
    )
    target_link_libraries(linux_uvc_camera PRIVATE
        PkgConfig::OPUS
        PkgConfig::ALSA
    )
    target_compile_definitions(linux_uvc_camera PRIVATE LINUX_UVC_HAS_AUDIO=1)
endif()

# HD/4K IDR fragments overflow the default out-queue and the pacer burst
# cap — bump both globally via the config override. It must apply to both
# the example and the library so struct layouts stay in sync, hence PUBLIC
# on the nanortc target. Build this example in its own build dir so this is
# the only NANORTC_CONFIG_FILE set on the shared nanortc target.
target_compile_definitions(nanortc PUBLIC
    NANORTC_CONFIG_FILE="nanortc_app_config.h"
)
target_include_directories(nanortc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
