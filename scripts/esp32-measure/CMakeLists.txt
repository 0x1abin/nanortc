# Minimal ESP-IDF project for measuring nanortc Flash/RAM sizes
# Usage: idf.py set-target esp32s3 && idf.py build

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_measure)
