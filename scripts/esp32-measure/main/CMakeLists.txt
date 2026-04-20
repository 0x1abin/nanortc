idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nanortc
)

# Mirror the profile-header override from the nanortc component so
# sizeof(nanortc_t) evaluated here sees the same overrides. `-include`
# and the path are separate arguments so CMake quotes the path safely
# against spaces / shell metacharacters.
if(DEFINED ENV{NANORTC_PROFILE_HDR})
    set(_nanortc_profile_hdr "$ENV{NANORTC_PROFILE_HDR}")
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -include "${_nanortc_profile_hdr}")
endif()
