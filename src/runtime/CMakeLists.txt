# libbro: the engine as one shared library, and the executables that run it.
#
# libbro.so / libbro.dll / libbro.dylib holds all of bro: the static libraries
# under src/ and the siblings linked into them. bro, bro-headless, bro-server
# and every installed app's own executable (bro-app-stub, stamped per app;
# engine/app_exe.h) are a few lines each that call into it (bro_entry.h), so
# every bro process maps one copy of the engine's code. BUILDING.md, "The
# engine library", has the reasoning.
#
# What crosses the boundary:
#   * everywhere, bro_entry.h's C functions;
#   * on ELF and Mach-O, the whole C++ surface as well: the library is linked
#     from whole archives with default visibility, so an embedder that drives
#     the Engine from C++ (helm) links it like any shared library;
#   * on Windows, the C functions only. The engine's C++ surface (and the
#     siblings inside it) is never annotated for export: a DLL's export table
#     cannot hold it (65535 entries), and an annotated surface is a
#     maintenance burden for every header in eleven repositories. A Windows
#     embedder links the static libraries instead (docs/embedding.md).

set(BRO_RUNTIME_SOURCES
    entry.cpp
    bro_main.cpp
    headless_main.cpp
    server_main.cpp
    app_exe_main.cpp
)

add_library(libbro SHARED ${BRO_RUNTIME_SOURCES})
set_target_properties(libbro PROPERTIES
    OUTPUT_NAME bro
    PREFIX lib
    IMPORT_PREFIX lib
    # Beside the executables, where bronze's runtime is staged too.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)
target_compile_definitions(libbro PRIVATE BRO_RUNTIME_BUILD)
target_include_directories(libbro PUBLIC ${bro_SOURCE_DIR}/src)

# Every static library bro builds under src/ that the engine links. Linked
# normally (which brings each one's own dependencies) and, off Windows, also
# as whole archives so the library carries the full C++ surface rather than
# only what the entry points reach.
set(_bro_engine_archives
    bro_util bro_platform bro_svg bro_bidi bro_glsl bro_render bro_dom bro_layout
    bro_canvas bro_webgl bro_audio_inference bro_steam bro_engine bro_bronze_host bro_bronze_js)
foreach(_t bro_tile bro_terminal bro_a11y bro_compositor bro_physics bro_scene bro_net bro_video bro_webp bro_avif)
    if(TARGET ${_t})
        list(APPEND _bro_engine_archives ${_t})
    endif()
endforeach()
target_link_libraries(libbro PRIVATE ${_bro_engine_archives})
if(BRO_WITH_GAMEAI)
    target_link_libraries(libbro PRIVATE brogameagent)
endif()
target_link_libraries(libbro PUBLIC bronze::runtime_shared)
if(WIN32)
    target_link_libraries(libbro PRIVATE ws2_32)
elseif(APPLE)
    foreach(_t IN LISTS _bro_engine_archives)
        target_link_options(libbro PRIVATE "LINKER:-force_load,$<TARGET_FILE:${_t}>")
    endforeach()
else()
    set(_bro_whole "")
    foreach(_t IN LISTS _bro_engine_archives)
        list(APPEND _bro_whole "$<TARGET_FILE:${_t}>")
    endforeach()
    # One option, so CMake neither splits nor de-duplicates the bracket.
    string(JOIN "," _bro_whole_csv ${_bro_whole})
    target_link_options(libbro PRIVATE
        "LINKER:--whole-archive,${_bro_whole_csv},--no-whole-archive"
        # Calls inside the library bind inside it: no PLT hop for the
        # engine's own functions, and far fewer symbol lookups at load.
        "LINKER:-Bsymbolic-functions")
endif()
bro_bronze_stage_runtime(libbro)

# The executables and the library find libbro and bronze's runtime beside
# themselves, so a packaged copy runs from wherever it is unpacked (the build
# tree's absolute rpath stays for running in place).
if(APPLE)
    set(_bro_origin_rpath "@loader_path")
elseif(NOT WIN32)
    set(_bro_origin_rpath "$ORIGIN")
endif()
if(_bro_origin_rpath)
    set_property(TARGET libbro APPEND PROPERTY BUILD_RPATH "${_bro_origin_rpath}")
    set_property(TARGET libbro APPEND PROPERTY INSTALL_RPATH "${_bro_origin_rpath}")
endif()

# bro::libbro: the library plus the engine's headers and feature definitions,
# for an embedder in this build that drives the Engine from C++ (ELF and
# Mach-O; see above). COMPILE_ONLY carries the usage requirements of the
# static libraries without linking them a second time.
add_library(bro_runtime_usage INTERFACE)
target_link_libraries(bro_runtime_usage INTERFACE bro_engine bro_bronze_host bro_bronze_js)
add_library(bro_libbro INTERFACE)
target_link_libraries(bro_libbro INTERFACE libbro bronze::runtime_shared)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
    target_link_libraries(bro_libbro INTERFACE "$<COMPILE_ONLY:bro_runtime_usage>")
endif()
add_library(bro::libbro ALIAS bro_libbro)

# bro-config.cmake in the build tree: the same target for an embedder built
# OUTSIDE this tree (helm with -Dbro_DIR=<this build>), so it maps the very
# libbro.so the apps of this build map. Include directories and definitions
# are read off the engine's targets, transitively, at generate time.
set(_bro_cfg_dir "${CMAKE_BINARY_DIR}")
set(_bro_features "")
foreach(_f IN LISTS BRO_FEATURE_FLAGS)
    if(BRO_WITH_${_f})
        string(APPEND _bro_features "set(BRO_WITH_${_f} ON)\n")
    else()
        string(APPEND _bro_features "set(BRO_WITH_${_f} OFF)\n")
    endif()
endforeach()
if(CMAKE_CONFIGURATION_TYPES)
    set(_bro_cfg_file "${_bro_cfg_dir}/bro-config-$<CONFIG>.cmake")
else()
    set(_bro_cfg_file "${_bro_cfg_dir}/bro-config-build.cmake")
endif()
set(_bro_cfg_content
"# Generated by bro (src/runtime/CMakeLists.txt): the engine library of the
# build tree at ${CMAKE_BINARY_DIR}.
if(NOT TARGET bro::libbro)
    add_library(bro::libbro SHARED IMPORTED)
    set_target_properties(bro::libbro PROPERTIES
        IMPORTED_LOCATION \"$<TARGET_FILE:libbro>\"
        IMPORTED_IMPLIB \"$<TARGET_LINKER_FILE:libbro>\"
        INTERFACE_INCLUDE_DIRECTORIES \"$<TARGET_PROPERTY:bro_runtime_usage,INTERFACE_INCLUDE_DIRECTORIES>\"
        INTERFACE_COMPILE_DEFINITIONS \"$<TARGET_PROPERTY:bro_runtime_usage,INTERFACE_COMPILE_DEFINITIONS>\"
        INTERFACE_LINK_LIBRARIES \"$<TARGET_FILE:bronze_runtime_shared>\")
endif()
set(BRO_SOURCE_DIR \"${bro_SOURCE_DIR}\")
set(BRO_BINARY_DIR \"${CMAKE_BINARY_DIR}\")
set(BRO_RUNTIME_DIR \"$<TARGET_FILE_DIR:libbro>\")
set(BRO_VERSION \"${PROJECT_VERSION}\")
${_bro_features}")
file(GENERATE OUTPUT "${_bro_cfg_file}" CONTENT "${_bro_cfg_content}" TARGET libbro)
if(CMAKE_CONFIGURATION_TYPES)
    file(WRITE "${_bro_cfg_dir}/bro-config.cmake"
"# Generated by bro: picks the configuration the embedder builds (BRO_CONFIG,
# default Release).
if(NOT DEFINED BRO_CONFIG)
    set(BRO_CONFIG Release)
endif()
include(\"\${CMAKE_CURRENT_LIST_DIR}/bro-config-\${BRO_CONFIG}.cmake\")
")
else()
    file(WRITE "${_bro_cfg_dir}/bro-config.cmake"
"# Generated by bro.
include(\"\${CMAKE_CURRENT_LIST_DIR}/bro-config-build.cmake\")
")
endif()

# ---- the executables ------------------------------------------------------------

if(BRO_BUILD_EXECUTABLES)
    function(bro_add_runtime_exe name entry)
        add_executable(${name} ${ARGN} exe_main.cpp)
        target_compile_definitions(${name} PRIVATE BRO_EXE_ENTRY=${entry} BRO_RUNTIME_SHARED)
        target_link_libraries(${name} PRIVATE libbro)
        target_include_directories(${name} PRIVATE ${bro_SOURCE_DIR}/src)
        bro_bronze_stage_runtime(${name})
        if(_bro_origin_rpath)
            set_property(TARGET ${name} APPEND PROPERTY BUILD_RPATH "${_bro_origin_rpath}")
            set_property(TARGET ${name} APPEND PROPERTY INSTALL_RPATH "${_bro_origin_rpath}")
        endif()
        if(MSVC)
            # The main thread's stack is the executable's to declare: 8 MB.
            target_link_options(${name} PRIVATE /STACK:8388608)
            # Common-Controls v6 and the Windows 10/11 compatibility section.
            target_sources(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bro.manifest)
        endif()
    endfunction()

    # WIN32 = /SUBSYSTEM:WINDOWS: no console window on double-click (the
    # engine borrows the parent's console when started from one).
    bro_add_runtime_exe(bro bro_run_windowed WIN32)
    bro_add_runtime_exe(bro-headless bro_run_headless)
    bro_add_runtime_exe(bro-server bro_run_server)
    if(MSVC)
        # GUI subsystem but keep int main() as the entry point.
        target_link_options(bro PRIVATE /ENTRY:mainCRTStartup)
    endif()

    # bro-app-stub: the template every installed app's executable is copied
    # from (app_stub.cpp). It links nothing of bro's: it loads libbro at run
    # time from the path stamped into it.
    add_executable(bro-app-stub WIN32 app_stub.cpp)
    target_include_directories(bro-app-stub PRIVATE ${bro_SOURCE_DIR}/src)
    target_link_libraries(bro-app-stub PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(bro-app-stub libbro)
    if(MSVC)
        target_link_options(bro-app-stub PRIVATE /STACK:8388608 /ENTRY:mainCRTStartup)
        target_sources(bro-app-stub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bro.manifest)
    endif()
endif()
