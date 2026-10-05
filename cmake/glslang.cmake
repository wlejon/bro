# glslang.cmake — the one GLSL -> SPIR-V compiler bro uses, linked in-process.
#
# Every shader bro compiles goes through glslang: the 3D scene's built-in
# shaders at build time (via the bro_spirv_embed tool), and the clipmap
# terrain, custom scene shaders and WebGL programs at run time (via
# render/glsl_compiler.h). One compiler, one set of rules, on every platform.
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

# bro_spirv_header(<out_var> <gen_dir> <glsl_file> <vert|frag|comp>
#                  [NAME <variant>] [DEFINES <define>...])
#
# Compiles <glsl_file> at build time with bro_spirv_embed (src/render/tools)
# into <gen_dir>/<name>.spv.h and appends that header to <out_var>, for a
# target to list among its sources and #include. DEFINES are #defined (to 1)
# after the #version line; a variant built that way names its header with
# NAME (<variant>.spv.h) so it does not collide with the plain build.
#
# A cross build cannot run a tool it built for the target, so it names a
# host build of the tool with -DBRO_SPIRV_EMBED=<path>.
set(BRO_SPIRV_EMBED "" CACHE FILEPATH "Host bro_spirv_embed, for cross builds")
function(bro_spirv_header out_var gen_dir glsl_file stage)
    cmake_parse_arguments(_spv "" "NAME" "DEFINES" ${ARGN})
    get_filename_component(_name "${glsl_file}" NAME)
    if(_spv_NAME)
        set(_name "${_spv_NAME}")
    endif()
    set(_out "${gen_dir}/${_name}.spv.h")
    if(BRO_SPIRV_EMBED)
        set(_tool "${BRO_SPIRV_EMBED}")
        set(_tool_dep "${BRO_SPIRV_EMBED}")
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "bro: a cross build compiles its shaders with a host tool; "
                            "build bro_spirv_embed for the host and pass -DBRO_SPIRV_EMBED=<path>")
    else()
        set(_tool "$<TARGET_FILE:bro_spirv_embed>")
        set(_tool_dep bro_spirv_embed)
    endif()
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND "${_tool}" ${stage} "${glsl_file}" "${_out}" ${_spv_DEFINES}
        DEPENDS "${glsl_file}" ${_tool_dep}
        COMMENT "SPIR-V ${_name} -> ${_name}.spv.h"
        VERBATIM
    )
    set(${out_var} ${${out_var}} "${_out}" PARENT_SCOPE)
endfunction()
