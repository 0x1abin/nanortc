# nanortc — Benchmark suite
#
# Build: cmake -B build -DNANORTC_BUILD_BENCHMARKS=ON -DNANORTC_CRYPTO=openssl \
#              -DNANORTC_FEATURE_DATACHANNEL=ON -DNANORTC_FEATURE_AUDIO=ON -DNANORTC_FEATURE_VIDEO=ON
#        cmake --build build -j$(nproc)
#        ctest --test-dir build -R bench_ --output-on-failure

# All benchmarks link against the nanortc library and use bench_common.h
# Tests also include internal headers (src/) for micro-benchmarks.

if(NOT BUILD_TESTING)
    enable_testing()
endif()

# Helper: add a benchmark executable
function(add_bench NAME)
    add_executable(${NAME} ${NAME}.c)
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/crypto
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_link_libraries(${NAME} PRIVATE nanortc)
    set_target_properties(${NAME} PROPERTIES C_STANDARD 99)
    # Benchmarks are allowed to use malloc (for stats collection)
    target_compile_options(${NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES LABELS "benchmark" TIMEOUT 120)
endfunction()

# --- Core benchmarks (always built) ---
add_bench(bench_connect)
add_bench(bench_concurrent)

# --- DataChannel benchmarks ---
if(NANORTC_FEATURE_DATACHANNEL)
    add_bench(bench_dc_throughput)
    add_bench(bench_dc_latency)
endif()

# --- Media benchmarks ---
if(NANORTC_FEATURE_AUDIO OR NANORTC_FEATURE_VIDEO)
    add_bench(bench_rtp)
    add_bench(bench_srtp)
endif()
