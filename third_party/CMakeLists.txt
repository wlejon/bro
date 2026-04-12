# --- QuickJS ---
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
add_subdirectory(quickjs EXCLUDE_FROM_ALL)

# --- FastNoise2 (SIMD noise generation) ---
# Used directly by bro's terrain system. The source lives inside brokit's
# third_party; brokit guards its own add_subdirectory with if(NOT TARGET FastNoise)
# so this won't conflict.
if(NOT TARGET FastNoise)
    set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "" FORCE)
    set(FASTNOISE2_TESTS OFF CACHE BOOL "" FORCE)
    # Resolve via brokit's tree (standalone or submodule).
    set(_BROKIT_ROOT "${CMAKE_SOURCE_DIR}/../brokit")
    if(NOT EXISTS "${_BROKIT_ROOT}/CMakeLists.txt")
        set(_BROKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/brokit")
    endif()
    add_subdirectory("${_BROKIT_ROOT}/third_party/FastNoise2"
                     "${CMAKE_BINARY_DIR}/FastNoise2" EXCLUDE_FROM_ALL)
endif()

# --- brokit (JS runtime APIs) ---
# Prefer the standalone repo at ../brokit for local development.
# Falls back to the git submodule at third_party/brokit.
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "Path to standalone brokit repo")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    message(STATUS "brokit: using standalone repo at ${BROKIT_DIR}")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
else()
    message(STATUS "brokit: using submodule at third_party/brokit")
    add_subdirectory(brokit EXCLUDE_FROM_ALL)
endif()

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

# --- stb (single-file image loading + writing) ---
add_library(stb_image STATIC stb/stb_image.cpp stb/stb_image_write.cpp)
target_include_directories(stb_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# --- broaudio (audio engine library) ---
# Prefer the standalone repo at ../broaudio for local development.
# Falls back to the git submodule at third_party/broaudio.
set(BROAUDIO_DIR "${CMAKE_SOURCE_DIR}/../broaudio" CACHE PATH "Path to standalone broaudio repo")
if(EXISTS "${BROAUDIO_DIR}/CMakeLists.txt")
    message(STATUS "broaudio: using standalone repo at ${BROAUDIO_DIR}")
    add_subdirectory("${BROAUDIO_DIR}" "${CMAKE_BINARY_DIR}/broaudio" EXCLUDE_FROM_ALL)
else()
    message(STATUS "broaudio: using submodule at third_party/broaudio")
    add_subdirectory(broaudio EXCLUDE_FROM_ALL)
endif()

# --- qjsbind (C++20 QuickJS binding library, header-only) ---
# Prefer the standalone repo at ../qjsbind for local development.
set(QJSBIND_DIR "${CMAKE_SOURCE_DIR}/../qjsbind" CACHE PATH "Path to standalone qjsbind repo")
if(EXISTS "${QJSBIND_DIR}/CMakeLists.txt")
    message(STATUS "qjsbind: using standalone repo at ${QJSBIND_DIR}")
    add_subdirectory("${QJSBIND_DIR}" "${CMAKE_BINARY_DIR}/qjsbind" EXCLUDE_FROM_ALL)
else()
    message(STATUS "qjsbind: using submodule at third_party/qjsbind")
    add_subdirectory(qjsbind EXCLUDE_FROM_ALL)
endif()

# --- bromesh (mesh generation + manipulation library) ---
# Prefer the standalone repo at ../bromesh for local development.
# Falls back to the git submodule at third_party/bromesh.
set(BROMESH_DIR "${CMAKE_SOURCE_DIR}/../bromesh" CACHE PATH "Path to standalone bromesh repo")
if(EXISTS "${BROMESH_DIR}/CMakeLists.txt")
    message(STATUS "bromesh: using standalone repo at ${BROMESH_DIR}")
    add_subdirectory("${BROMESH_DIR}" "${CMAKE_BINARY_DIR}/bromesh" EXCLUDE_FROM_ALL)
else()
    message(STATUS "bromesh: using submodule at third_party/bromesh")
    add_subdirectory(bromesh EXCLUDE_FROM_ALL)
endif()

# --- Jolt Physics ---
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # match bro's /MD runtime
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)          # don't clobber bro's flags
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)          # avoid -Werror on Jolt internals
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)  # LTO handled by bro's build
set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(jolt/Build EXCLUDE_FROM_ALL)

# --- GameNetworkingSockets (via vcpkg) ---
find_package(GameNetworkingSockets CONFIG REQUIRED)

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)

# --- Skia SVG module (compiled from source against pre-built Skia) ---
include(skia/skia_svg.cmake)
