cmake_minimum_required(VERSION 3.24)

# On Apple Silicon, derive triplet / target arch from the *hardware* rather
# than the process arch, so an Intel (Rosetta) cmake invocation doesn't
# silently pick x64-osx / x86_64 and fail to link against arm64 vcpkg
# artifacts. Runs before project() so CMAKE_OSX_ARCHITECTURES takes effect.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND sysctl -n hw.optional.arm64
        OUTPUT_VARIABLE _bro_host_is_arm64
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_bro_host_is_arm64 STREQUAL "1")
        if(NOT DEFINED CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "")
            set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
        endif()
        if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
            set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "")
        endif()
    endif()
endif()

# vcpkg toolchain. Resolution order:
#   1. -DCMAKE_TOOLCHAIN_FILE=... on the command line (respected as-is)
#   2. $VCPKG_ROOT environment variable
#   3. Auto-detect at common install locations (so a fresh `cmake -B build-xxx`
#      works on machines where vcpkg lives in a predictable place).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(_bro_vcpkg_root "$ENV{VCPKG_ROOT}")
    else()
        foreach(_candidate
                "$ENV{HOME}/vcpkg"
                "$ENV{HOME}/src/vcpkg"
                "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg"
                "/opt/vcpkg"
                "/usr/local/vcpkg")
            if(EXISTS "${_candidate}/scripts/buildsystems/vcpkg.cmake")
                set(_bro_vcpkg_root "${_candidate}")
                message(STATUS "vcpkg: auto-detected at ${_bro_vcpkg_root}")
                break()
            endif()
        endforeach()
    endif()
    if(DEFINED _bro_vcpkg_root)
        set(CMAKE_TOOLCHAIN_FILE "${_bro_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()

# Submodule preflight: a `git clone` without --recursive leaves the third_party
# submodule dirs empty and produces cryptic downstream failures. Only check
# non-sibling submodules — the brokit/htmlayout/broaudio/bromesh/qjsbind/
# brogameagent deps fall back to sibling repos at ../<name> (see third_party/
# CMakeLists.txt), so it's fine if their third_party/<name> dirs are empty.
foreach(_bro_submodule IN ITEMS quickjs SDL jolt)
    set(_bro_submodule_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_bro_submodule}")
    file(GLOB _bro_submodule_contents "${_bro_submodule_dir}/*")
    if(_bro_submodule_contents STREQUAL "")
        message(FATAL_ERROR
            "third_party/${_bro_submodule} is empty — submodules are not initialized.\n"
            "Run:  git submodule update --init --recursive")
    endif()
endforeach()

project(bro VERSION 0.1.0 LANGUAGES C CXX)

# Unify executable output. Multi-config (Visual Studio) auto-appends the
# config subdir → build/Debug/, build/Release/. Single-config (Ninja) drops
# binaries directly in build/. All targets (bro, bro-headless, bro-server)
# land together so DLLs only need to be staged once.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
else()
    # Embedded JS bundles contain UTF-8 bytes > 127; require unsigned char
    # so string literals don't trip narrowing-conversion errors on platforms
    # where `char` is signed (Apple clang).
    add_compile_options(-funsigned-char)
endif()

add_subdirectory(third_party)

add_subdirectory(src)
