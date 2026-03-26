# nanortc test targets
# Each test_*.c file becomes its own executable

file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE nanortc)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
