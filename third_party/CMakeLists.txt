# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- htmlayout (CSS + layout library, sibling project) ---
add_subdirectory(${CMAKE_SOURCE_DIR}/../htmlayout htmlayout_build EXCLUDE_FROM_ALL)

# --- SDL3 (fall back to submodule if not found via vcpkg/system) ---
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not found -- building from submodule")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    add_subdirectory(SDL EXCLUDE_FROM_ALL)
else()
    message(STATUS "SDL3 found via find_package")
endif()

# --- glad2 (OpenGL 3.3 Core loader) ---
add_subdirectory(glad EXCLUDE_FROM_ALL)

# --- stb (single-file image loading) ---
add_library(stb_image STATIC stb/stb_image.cpp)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)
