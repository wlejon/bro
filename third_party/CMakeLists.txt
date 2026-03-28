# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- LiteHTML ---
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(litehtml EXCLUDE_FROM_ALL)

# --- Vulkan Headers (fallback if no SDK) ---
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found - using bundled Vulkan-Headers")
    add_library(vulkan_headers INTERFACE)
    target_include_directories(vulkan_headers INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include
    )
    add_library(Vulkan::Headers ALIAS vulkan_headers)
endif()

# --- SDL3 (fall back to submodule if not found via vcpkg/system) ---
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found -- building from submodule")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory(SDL EXCLUDE_FROM_ALL)
else()
    message(STATUS "SDL3 found via find_package")
endif()

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)
