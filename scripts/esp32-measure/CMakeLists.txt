# Minimal ESP-IDF project for measuring nanortc Flash/RAM sizes
# Usage: idf.py set-target esp32s3 && idf.py build

cmake_minimum_required(VERSION 3.16)

# The component name is derived from the directory that holds CMakeLists.txt.
# When building from a git worktree (e.g. .claude/worktrees/<name>), the repo
# root is not named "nanortc", so allow an override via NANORTC_COMPONENT_DIR
# (typically a symlink like /tmp/nanortc-components/nanortc -> worktree path).
if(DEFINED ENV{NANORTC_COMPONENT_DIR})
    set(EXTRA_COMPONENT_DIRS "$ENV{NANORTC_COMPONENT_DIR}")
else()
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_measure)
