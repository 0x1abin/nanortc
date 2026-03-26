add_executable(linux_media_send
    main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/run_loop_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/signaling_stdin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/media_source_file.c
)

target_include_directories(linux_media_send PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(linux_media_send PRIVATE nanortc)
