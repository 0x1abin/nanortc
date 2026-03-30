# ESP-IDF project CMakeLists.txt for nanortc ESP32 Audio example
#
# Build: cd examples/esp32_audio && idf.py build
# Flash: idf.py flash monitor

cmake_minimum_required(VERSION 3.16)

# nanortc is an extra component (parent repo root)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_audio)
