# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- LiteHTML ---
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(litehtml EXCLUDE_FROM_ALL)

# --- Vulkan Headers (from Khronos) ---
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found - using bundled Vulkan-Headers + runtime loading")
    add_library(vulkan_headers INTERFACE)
    target_include_directories(vulkan_headers INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include
    )
    add_library(Vulkan::Headers ALIAS vulkan_headers)
    add_library(vulkan_stub INTERFACE)
    target_include_directories(vulkan_stub INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include
    )
    add_library(Vulkan::Vulkan ALIAS vulkan_stub)
else()
    message(STATUS "Vulkan SDK found: ${Vulkan_LIBRARY}")
endif()

# --- SDL3 (only build from submodule if not found as a system package) ---
if(NOT BRO_SDL3_FROM_PACKAGE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    add_subdirectory(SDL EXCLUDE_FROM_ALL)
endif()

# --- Skia (pre-built, imported) ---
include(skia/skia.cmake)
