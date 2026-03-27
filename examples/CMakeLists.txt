# nanortc examples — Linux host only (not for ESP-IDF)

add_subdirectory(linux_datachannel)
add_subdirectory(browser_interop)

if(PROFILE_NUM GREATER_EQUAL 2)
    add_subdirectory(linux_media_send)
endif()
