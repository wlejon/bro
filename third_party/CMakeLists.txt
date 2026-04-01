# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- htmlayout (CSS + layout library) ---
# Prefer the standalone repo at ../htmlayout for local development (edit once, no sync dance).
# Falls back to the git submodule at third_party/htmlayout.
set(HTMLAYOUT_DIR "${CMAKE_SOURCE_DIR}/../htmlayout" CACHE PATH "Path to standalone htmlayout repo")
if(EXISTS "${HTMLAYOUT_DIR}/CMakeLists.txt")
    message(STATUS "htmlayout: using standalone repo at ${HTMLAYOUT_DIR}")
    add_subdirectory("${HTMLAYOUT_DIR}" "${CMAKE_BINARY_DIR}/htmlayout" EXCLUDE_FROM_ALL)
else()
    message(STATUS "htmlayout: using submodule at third_party/htmlayout")
    add_subdirectory(htmlayout EXCLUDE_FROM_ALL)
endif()

# --- SDL3 (built from submodule) ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL EXCLUDE_FROM_ALL)

# --- glad2 (OpenGL 3.3 Core loader) ---
add_subdirectory(glad EXCLUDE_FROM_ALL)

# --- stb (single-file image loading) ---
add_library(stb_image STATIC stb/stb_image.cpp)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)
