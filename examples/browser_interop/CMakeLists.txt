add_executable(browser_interop
    main.c
    http_signaling.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/run_loop_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/media_source_file.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/ice_server_resolve.c
)

target_include_directories(browser_interop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(browser_interop PRIVATE nanortc)
