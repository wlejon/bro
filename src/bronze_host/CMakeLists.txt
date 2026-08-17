# bronze host layer — only ever configured under BRO_WITH_BRONZE=ON
# (src/CMakeLists.txt gates the add_subdirectory), so nothing here can touch
# the default build.

# --- Resolve the bronze compiler as a sibling working tree ---
# Same resolution as the bro-* siblings (third_party/CMakeLists.txt's
# bro_default_sibling_dir — defined before src/ is processed, function scope
# is global), but with NO submodule fallback: bronze is a peer project, not a
# vendored dependency, and a silent fallback would build a tree that is not
# the one being developed. A missing checkout is a configure error naming the
# path it looked at.
bro_default_sibling_dir(bronze _bronze_default)
set(BRONZE_DIR "${_bronze_default}" CACHE PATH "bronze AOT compiler source tree")
if(NOT EXISTS "${BRONZE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "BRO_WITH_BRONZE=ON needs a bronze checkout, and none was found at\n"
        "    ${BRONZE_DIR}\n"
        "Clone it as a sibling of bro (../bronze) or pass -DBRONZE_DIR=<path>. "
        "There is no submodule fallback, deliberately.")
endif()

# bronze builds its runtime as a shared library only when asked, and its own
# default for the question is "am I the top-level project" — which is OFF here,
# where it is a subdirectory. bro always needs it ON: an app arrives as a
# module bro LOADS, and host and module must bind to one runtime image. FORCE
# rather than a default, because there is no working configuration of bro with
# it off, so an inherited OFF in a cache would be a link error a long way from
# its cause.
set(BRONZE_BUILD_SHARED_RUNTIME ON CACHE BOOL "" FORCE)

# The codegen backend, so this tree can COMPILE an app and not only run one.
# bronze's own default is OFF and its reason is good — the runtime does not
# need LLVM and neither does a host that only loads finished modules — but a
# bro checkout is where apps get built and where the bronze_host checks compile
# their own probes, and a check that cannot produce its subject is a check that
# reports "skipped" forever.
#
# A plain cache default rather than FORCE: unlike the shared runtime, OFF here
# is a real configuration (run pre-built apps, build no new ones), so
# -DBRONZE_WITH_LLVM=OFF stays available to anyone who wants a lighter tree.
# bronze searches the usual install locations itself when LLVM_DIR is unset.
#
# Being a default and not a FORCE, it applies to a FRESH cache only: a tree
# configured before this line keeps whatever it already decided, and turning
# the compiler on there is one -DBRONZE_WITH_LLVM=ON. That is the normal
# meaning of a cached default, and the alternative — FORCE — would take the
# choice away from every tree on every reconfigure to save that one flag.
set(BRONZE_WITH_LLVM ON CACHE BOOL "Build bronze's LLVM codegen backend (the app compiler)")

# EXCLUDE_FROM_ALL: only the bronze targets our libraries name get built;
# bronze's CLI, backend, and test binaries stay out of bro's default target.
# Note bronze's own configure requires doctest (find_package REQUIRED) — the
# configure fails there, loudly, if the toolchain cannot provide it.
add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze" EXCLUDE_FROM_ALL)

# --- The binding library ---
add_library(bro_bronze_host STATIC
    gl_constants.cpp
    gl_state.cpp
    gl_buffers.cpp
    gl_shaders.cpp
    gl_textures.cpp
    gl_framebuffers.cpp
    gl_queries.cpp
    gl_context.cpp
    app_module.cpp
    dom_globals.cpp
    host_element.cpp
    host_node.cpp
    host_events.cpp
    host_dom_events.cpp
    host_timers.cpp
    host_image.cpp
    host_platform.cpp
    host_xhr.cpp
    host_fetch.cpp
    host_file.cpp
    host_abort.cpp
    host_observers.cpp
    host_parser.cpp
    host_video.cpp
    host_audio.cpp
)
target_include_directories(bro_bronze_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The runtime is a DLL now, and bronze builds it into its own output directory
# (BRONZE_SHARED_RUNTIME_DIR) while bro's executables land in bro's. On Windows
# that is the difference between a program and one that will not start: the
# loader looks beside the .exe, not wherever the link-time import library came
# from. So every executable that links this layer gets the runtime copied next
# to it as a post-build step.
#
# A function rather than a property on bro_bronze_host, because the copy has to
# name a DESTINATION and a static library does not have one — the directory
# that matters belongs to whichever executable ends up linking it.
function(bro_bronze_stage_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bronze_runtime_shared> $<TARGET_FILE_DIR:${target}>
        COMMENT "Staging bronze's shared runtime beside ${target}"
        VERBATIM)
endfunction()
# broimage is named explicitly rather than inherited: host_image.cpp includes
# broimage/decode.h directly, and a dependency that only works because someone
# else happens to link it is one refactor away from a broken build.
#
# bronze::runtime_shared and NOT bronze::embed + bronze::runtime, which is the
# one link mistake this layer cannot survive: the static archives carry their
# own copy of state bronze documents as "process-wide, owned by ONE translation
# unit" (runtime/rt_state.h), so a process holding both has two heaps, two
# exception cells and two key registries. A value the loaded module allocated
# would be an address in a semispace bro's collector is free to reuse. embed is
# INSIDE the shared image — the host↔runtime boundary is a C++ one no export
# list could carry — so this one target supplies both halves.
target_link_libraries(bro_bronze_host
    PUBLIC bro_engine bro_webgl glad broimage::broimage bronze::runtime_shared
)

# --- No app targets here, deliberately ---
# What used to live below this line was the link-time app binding:
# BRO_BRONZE_APP_OBJ, BRO_BRONZE_BUILD_APP, a bro_bronze_add_host() function and
# a BRO_BRONZE_APPS list that produced one bro-bronze-host-<name> executable per
# compiled app. All of it existed to answer a question the build no longer asks.
#
# An app is a directory that carries its own module now (app.dll / app.so /
# app.dylib, src/bronze_host/app_module.h), which the stock bro and bro-headless
# load at startup. "Another app" is another folder, not another link of
# host_main.cpp with a different object beside it — so there is no app-shaped
# input to this build, no per-app target, and nothing here for an app author to
# configure.
#
# Building an app is `bronze build <entry>.js -o <appdir>/app.dll --emit-shared
# --host-globals src/bronze_host/web_host.globals`, and this tree can produce
# the compiler that does it (BRONZE_WITH_LLVM above). src/bronze_host/README.md
# has the sequence.
