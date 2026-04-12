set(COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_executable(browser_interop
    main.c
    ${COMMON}/http_signaling.c
    ${COMMON}/run_loop_linux.c
    ${COMMON}/media_source_file.c
    ${COMMON}/ice_server_resolve.c
)

target_include_directories(browser_interop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(browser_interop PRIVATE nanortc)
