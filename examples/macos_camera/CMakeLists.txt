# macos_camera — Real-time camera/mic → browser via nanortc (macOS only)

if(NOT APPLE)
    message(STATUS "macos_camera: skipping (not macOS)")
    return()
endif()

enable_language(OBJC)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

add_executable(macos_camera
    main.m
    av_capture.m
    vt_encoder.m
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/run_loop_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser_interop/http_signaling.c
)

target_include_directories(macos_camera PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser_interop
)

target_link_libraries(macos_camera PRIVATE
    nanortc
    PkgConfig::OPUS
    "-framework AVFoundation"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework VideoToolbox"
    "-framework Foundation"
)

# Override nanortc config for 1080p video (must apply to library + example
# so struct layouts match — hence PUBLIC on the nanortc target)
target_compile_definitions(nanortc PUBLIC
    NANORTC_CONFIG_FILE="nanortc_app_config.h"
)
target_include_directories(nanortc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Enable Objective-C ARC for .m files
set_source_files_properties(main.m av_capture.m vt_encoder.m
    PROPERTIES COMPILE_FLAGS "-fobjc-arc"
)
