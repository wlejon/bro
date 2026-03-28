cmake_minimum_required(VERSION 3.28)
project(bro VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Parallel compilation, conformance mode, UTF-8 source
    add_compile_options(/MP /permissive- /utf-8)
endif()

# --- Find system/vcpkg packages before descending into third_party ---
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    message(STATUS "SDL3 found via find_package (vcpkg or system)")
    set(BRO_SDL3_FROM_PACKAGE TRUE)
else()
    message(STATUS "SDL3 not found via find_package -- will build from submodule")
    set(BRO_SDL3_FROM_PACKAGE FALSE)
endif()

add_subdirectory(third_party)
add_subdirectory(src)
