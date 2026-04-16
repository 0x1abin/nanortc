# aiortc-based interop tests — Python-driven DataChannel E2E
#
# Build: cmake -B build -DNANORTC_BUILD_INTEROP_TESTS_PY=ON
#
# Produces the nanortc_peer_cli helper binary plus a ctest target that runs
# pytest against aiortc. Unlike the libdatachannel suite this does NOT require
# OpenSSL or a C++ compiler — the CLI uses the default nanortc crypto backend.
#
# SPDX-License-Identifier: MIT

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

# ---- CLI helper binary ----

add_executable(nanortc_peer_cli
    nanortc_peer_cli.c
    ${CMAKE_SOURCE_DIR}/examples/common/run_loop_linux.c
)

target_include_directories(nanortc_peer_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_SOURCE_DIR}/examples/common
)

target_link_libraries(nanortc_peer_cli PRIVATE
    nanortc
    pthread
)

set_target_properties(nanortc_peer_cli PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# ---- Python venv + aiortc install ----

set(VENV_DIR ${CMAKE_CURRENT_BINARY_DIR}/.venv)
set(VENV_READY ${VENV_DIR}/.ready)
set(REQUIREMENTS ${CMAKE_CURRENT_SOURCE_DIR}/requirements.txt)

if(APPLE)
    set(VENV_PYTHON ${VENV_DIR}/bin/python)
    set(VENV_PIP ${VENV_DIR}/bin/pip)
elseif(WIN32)
    set(VENV_PYTHON ${VENV_DIR}/Scripts/python.exe)
    set(VENV_PIP ${VENV_DIR}/Scripts/pip.exe)
else()
    set(VENV_PYTHON ${VENV_DIR}/bin/python)
    set(VENV_PIP ${VENV_DIR}/bin/pip)
endif()

add_custom_command(
    OUTPUT ${VENV_READY}
    COMMAND ${Python3_EXECUTABLE} -m venv ${VENV_DIR}
    COMMAND ${VENV_PIP} install -q --disable-pip-version-check -r ${REQUIREMENTS}
    COMMAND ${CMAKE_COMMAND} -E touch ${VENV_READY}
    DEPENDS ${REQUIREMENTS}
    COMMENT "Setting up Python venv for interop_py (pip install -r requirements.txt)"
    VERBATIM
)

add_custom_target(interop_py_venv ALL DEPENDS ${VENV_READY})
add_dependencies(nanortc_peer_cli interop_py_venv)

# ---- Register ctest target ----

add_test(
    NAME interop_py_datachannel
    COMMAND ${VENV_PYTHON} -m pytest -v -s
            ${CMAKE_CURRENT_SOURCE_DIR}/test_datachannel.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

set_tests_properties(interop_py_datachannel PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "NANORTC_PEER_CLI=$<TARGET_FILE:nanortc_peer_cli>;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
)
