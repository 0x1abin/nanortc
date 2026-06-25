# ESP-IDF project CMakeLists.txt for nanortc ESP32 Media example
#
# Streams pre-encoded H.264 video and/or Opus audio from flash to a browser.
# Select tracks via menuconfig (EXAMPLE_ENABLE_VIDEO / EXAMPLE_ENABLE_AUDIO).
#
# Build: cd examples/esp32_media && idf.py build
# Flash: idf.py flash monitor

cmake_minimum_required(VERSION 3.16)

# nanortc is an extra component (parent repo root)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_media)
