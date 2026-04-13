# nanortc fuzz testing harnesses
# Requires Clang with libFuzzer support
#
# Usage:
#   cmake -B build-fuzz -DNANORTC_BUILD_FUZZ=ON \
#         -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON \
#         -DNANORTC_CRYPTO=mbedtls -DCMAKE_C_COMPILER=clang
#   cmake --build build-fuzz -j$(nproc)
#   ./build-fuzz/tests/fuzz/fuzz_stun corpus/stun/

set(FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

# Core parsers (always available)
set(FUZZ_CORE_TARGETS fuzz_stun fuzz_sdp fuzz_addr fuzz_turn)

# DataChannel parsers
set(FUZZ_DC_TARGETS fuzz_sctp)

# Media parsers
set(FUZZ_MEDIA_TARGETS fuzz_rtp)

# Video parsers
set(FUZZ_VIDEO_TARGETS fuzz_h264 fuzz_h265 fuzz_bwe)

# Build core fuzz targets
foreach(target ${FUZZ_CORE_TARGETS})
    add_executable(${target} ${target}.c)
    target_link_libraries(${target} PRIVATE nanortc)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
    )
    target_compile_options(${target} PRIVATE ${FUZZ_FLAGS})
    target_link_options(${target} PRIVATE ${FUZZ_FLAGS})
endforeach()

# Build DC fuzz targets
if(NANORTC_FEATURE_DATACHANNEL)
    foreach(target ${FUZZ_DC_TARGETS})
        add_executable(${target} ${target}.c)
        target_link_libraries(${target} PRIVATE nanortc)
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/crypto
        )
        target_compile_options(${target} PRIVATE ${FUZZ_FLAGS})
        target_link_options(${target} PRIVATE ${FUZZ_FLAGS})
    endforeach()
endif()

# Build media fuzz targets
if(NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO)
    foreach(target ${FUZZ_MEDIA_TARGETS})
        add_executable(${target} ${target}.c)
        target_link_libraries(${target} PRIVATE nanortc)
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/crypto
        )
        target_compile_options(${target} PRIVATE ${FUZZ_FLAGS})
        target_link_options(${target} PRIVATE ${FUZZ_FLAGS})
    endforeach()
endif()

# Build video fuzz targets
if(NANORTC_FEATURE_VIDEO)
    foreach(target ${FUZZ_VIDEO_TARGETS})
        add_executable(${target} ${target}.c)
        target_link_libraries(${target} PRIVATE nanortc)
        target_include_directories(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/crypto
        )
        target_compile_options(${target} PRIVATE ${FUZZ_FLAGS})
        target_link_options(${target} PRIVATE ${FUZZ_FLAGS})
    endforeach()
endif()
