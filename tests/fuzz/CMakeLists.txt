# nanortc fuzz testing harnesses
#
# Harness sources are discovered automatically from fuzz_*.c. Feature-specific
# targets are skipped when their owning module is disabled; fuzz_reorder and
# fuzz_fec compile their codec source directly so those opt-in paths remain
# covered in the standard VIDEO fuzz profile.

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "NANORTC_BUILD_FUZZ requires LLVM Clang with libFuzzer support (not ${CMAKE_C_COMPILER_ID})")
endif()

set(FUZZ_COMPILE_FLAGS
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
)
set(FUZZ_LINK_FLAGS
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
)

# Instrument the library itself, not just each one-function harness. Without
# this, libFuzzer sees coverage only in LLVMFuzzerTestOneInput() and cannot
# guide mutations through the protocol parsers in libnanortc.a.
target_compile_options(nanortc PRIVATE ${FUZZ_COMPILE_FLAGS})
# The instrumented static archive may also be linked by ordinary test/example
# targets when NANORTC_BUILD_FUZZ is enabled alongside them. Propagate the
# sanitizer runtimes without libFuzzer's main so those consumers still link;
# fuzz harnesses replace this with their own -fsanitize=fuzzer link option.
target_link_options(nanortc INTERFACE
    -fsanitize=fuzzer-no-link,address,undefined
)

set(FUZZ_DC_TARGETS fuzz_sctp)
set(FUZZ_MEDIA_TARGETS fuzz_rtp)
set(FUZZ_VIDEO_TARGETS fuzz_h264 fuzz_bwe fuzz_twcc fuzz_reorder fuzz_fec)
set(FUZZ_H265_TARGETS fuzz_h265)
set(FUZZ_TURN_TARGETS fuzz_turn)

function(nanortc_configure_fuzz_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
    )
    target_compile_options(${target} PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(${target} PRIVATE ${FUZZ_LINK_FLAGS})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
endfunction()

file(GLOB FUZZ_HARNESS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_*.c"
)
set(NANORTC_FUZZ_TARGETS "")

foreach(source IN LISTS FUZZ_HARNESS_SOURCES)
    get_filename_component(target "${source}" NAME_WE)
    set(enabled TRUE)

    if("${target}" IN_LIST FUZZ_DC_TARGETS AND NOT NANORTC_FEATURE_DATACHANNEL)
        set(enabled FALSE)
    elseif("${target}" IN_LIST FUZZ_MEDIA_TARGETS AND
           NOT (NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO))
        set(enabled FALSE)
    elseif("${target}" IN_LIST FUZZ_VIDEO_TARGETS AND NOT NANORTC_FEATURE_VIDEO)
        set(enabled FALSE)
    elseif("${target}" IN_LIST FUZZ_H265_TARGETS AND NOT NANORTC_FEATURE_H265)
        set(enabled FALSE)
    elseif("${target}" IN_LIST FUZZ_TURN_TARGETS AND NOT NANORTC_FEATURE_TURN)
        set(enabled FALSE)
    endif()

    if(NOT enabled)
        continue()
    endif()

    if(target STREQUAL "fuzz_reorder")
        add_executable(${target}
            ${source}
            ${CMAKE_SOURCE_DIR}/src/nano_reorder.c
        )
        target_compile_definitions(${target} PRIVATE
            NANORTC_FEATURE_VIDEO=1
            NANORTC_FEATURE_VIDEO_REORDER=1
        )
    elseif(target STREQUAL "fuzz_fec")
        add_executable(${target}
            ${source}
            ${CMAKE_SOURCE_DIR}/src/nano_fec.c
        )
        target_compile_definitions(${target} PRIVATE
            NANORTC_FEATURE_VIDEO=1
            NANORTC_FEATURE_VIDEO_FEC=1
            NANORTC_FEATURE_VIDEO_REORDER=1
        )
    else()
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE nanortc)
    endif()

    nanortc_configure_fuzz_target(${target})
    list(APPEND NANORTC_FUZZ_TARGETS ${target})
endforeach()

list(SORT NANORTC_FUZZ_TARGETS)
message(STATUS "nanortc fuzz targets: ${NANORTC_FUZZ_TARGETS}")
