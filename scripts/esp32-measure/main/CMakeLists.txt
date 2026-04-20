idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nanortc
)

# Mirror the profile-header override from the nanortc component so
# sizeof(nanortc_t) evaluated here sees the same overrides.
if(DEFINED ENV{NANORTC_PROFILE_HDR})
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "SHELL:-include $ENV{NANORTC_PROFILE_HDR}")
endif()
