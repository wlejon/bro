# Skia - imported pre-built static library
#
# Populate third_party/skia/include/ and third_party/skia/lib/ with a Skia
# build before enabling this. Build instructions:
#   cd third_party/skia/src && python3 tools/git-sync-deps
#   bin/gn gen out/Release --args='...'  (see CLAUDE.md)
#   ninja -C out/Release skia

option(BRO_USE_SKIA "Link against pre-built Skia" OFF)

# Auto-detect: if lib exists, enable Skia
if(NOT DEFINED CACHE{BRO_USE_SKIA})
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/skia.lib" OR
       EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/libskia.a")
        set(BRO_USE_SKIA ON CACHE BOOL "Link against pre-built Skia" FORCE)
        message(STATUS "Skia library found — enabling BRO_USE_SKIA")
    endif()
endif()

if(BRO_USE_SKIA)
    add_library(skia STATIC IMPORTED GLOBAL)
    if(WIN32)
        set(_skia_lib "${CMAKE_CURRENT_LIST_DIR}/lib/skia.lib")
    else()
        set(_skia_lib "${CMAKE_CURRENT_LIST_DIR}/lib/libskia.a")
    endif()
    set_target_properties(skia PROPERTIES
        IMPORTED_LOCATION "${_skia_lib}"
    )
    # Skia headers use #include <include/core/...> so the include root is the parent
    target_include_directories(skia INTERFACE "${CMAKE_CURRENT_LIST_DIR}/src")
    # Skia's Vulkan backend needs the Vulkan loader
    if(TARGET Vulkan::Vulkan)
        target_link_libraries(skia INTERFACE Vulkan::Vulkan)
    elseif(WIN32)
        target_link_libraries(skia INTERFACE vulkan-1)
    endif()
else()
    add_library(skia INTERFACE)
endif()
