add_executable(linux_datachannel
    main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/run_loop_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/signaling_stdin.c
)

target_include_directories(linux_datachannel PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

target_link_libraries(linux_datachannel PRIVATE nanortc)
