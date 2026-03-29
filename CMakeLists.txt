cmake_minimum_required(VERSION 3.24)

# Auto-detect vcpkg if not already configured
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "D:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "D:/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(bro VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
endif()

# Find system packages at root scope so IMPORTED targets are globally visible
find_package(SDL3 CONFIG QUIET)
add_subdirectory(third_party)

# BRO_USE_SKIA is defined in third_party/skia/skia.cmake (option, default OFF).
# When Skia is not available, define BRO_NO_SKIA project-wide.
if(NOT BRO_USE_SKIA)
    add_compile_definitions(BRO_NO_SKIA)
endif()

add_subdirectory(src)
