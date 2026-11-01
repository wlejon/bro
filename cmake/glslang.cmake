# glslang.cmake — WebGL2's GLSL -> SPIR-V compiler, linked in-process.
#
# GLSL is web content's alone: WebGL programs compile through glslang at run
# time (webgl/glsl_compiler.h, and its C++ API for their reflection). bro's
# own GPU code is brokernel, compiled by brass.
#
# It is built from source, statically, at a pinned release, rather than found
# on the system: distro packages ship it as a shared library (or as shaderc's
# shared library) which a packaged bro would then have to carry, and Windows
# and macOS have no system copy at all. Built without SPIRV-Tools (no
# optimiser, which bro never asked for) and without the HLSL front end, it is
# a couple of hundred translation units and no further dependencies.
#
# Offline / pre-fetched: -DFETCHCONTENT_SOURCE_DIR_GLSLANG=<glslang checkout>
# uses that source tree instead of downloading (any tree at this tag).

include(FetchContent)

set(BRO_GLSLANG_VERSION 16.6.0)
FetchContent_Declare(glslang
    URL      https://github.com/KhronosGroup/glslang/archive/refs/tags/${BRO_GLSLANG_VERSION}.tar.gz
    URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# glslang's own switches, set as cache entries before it reads them so a
# reconfigure keeps them. Static regardless of BUILD_SHARED_LIBS.
set(ENABLE_OPT               OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL              OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES  OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_JS        OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS            OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL           OFF CACHE BOOL "" FORCE)
set(_bro_glslang_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(glslang)
set(BUILD_SHARED_LIBS ${_bro_glslang_shared})

if(NOT TARGET glslang::glslang OR NOT TARGET glslang::glslang-default-resource-limits)
    message(FATAL_ERROR "bro: glslang ${BRO_GLSLANG_VERSION} did not define glslang::glslang "
                        "and glslang::glslang-default-resource-limits")
endif()
