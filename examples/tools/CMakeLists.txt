find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

add_executable(opus_verify opus_verify.c)
target_link_libraries(opus_verify PRIVATE PkgConfig::OPUS)

add_executable(opus_gen_tone opus_gen_tone.c)
target_link_libraries(opus_gen_tone PRIVATE PkgConfig::OPUS m)
