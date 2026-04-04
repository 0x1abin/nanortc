# ESP-IDF project CMakeLists.txt for nanortc ESP32-P4 Camera example
#
# Build: cd examples/esp32_camera && idf.py set-target esp32p4 && idf.py build
# Flash: idf.py flash monitor

cmake_minimum_required(VERSION 3.16)

# nanortc is an extra component (parent repo root)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_camera)
