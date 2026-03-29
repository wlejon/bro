# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- LiteHTML ---
set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(litehtml EXCLUDE_FROM_ALL)

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

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)
