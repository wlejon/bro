# bronze host layer — only ever configured under BRO_WITH_BRONZE=ON
# (src/CMakeLists.txt gates the add_subdirectory), so nothing here can touch
# the default build.

# --- Resolve the bronze compiler: sibling working tree, else submodule ---
# Same resolution as the bro-* siblings (third_party/CMakeLists.txt's
# bro_default_sibling_dir — defined before src/ is processed, function scope is
# global): a standalone ../bronze wins, and third_party/bronze is the fallback.
#
# The fallback used to be absent, deliberately, on the grounds that bronze is a
# peer project rather than a vendored dependency and that quietly building some
# other tree is worse than failing. Both halves of that are still true; what
# changed is that CI, the nightly package and a fresh clone all need a bronze
# that arrives with the repo at a pinned commit, and a pin is exactly what a
# submodule is. So the fallback exists now and the objection is answered by
# making it LOUD instead: this always reports which of the two trees it picked,
# so a build against the submodule can never be mistaken for a build against
# the checkout you are editing.
bro_default_sibling_dir(bronze _bronze_default)
if(NOT EXISTS "${_bronze_default}/CMakeLists.txt")
    set(_bronze_default "${bro_SOURCE_DIR}/third_party/bronze")
endif()
set(BRONZE_DIR "${_bronze_default}" CACHE PATH "bronze AOT compiler source tree")
if(NOT EXISTS "${BRONZE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "BRO_WITH_BRONZE=ON needs a bronze checkout, and none was found at\n"
        "    ${BRONZE_DIR}\n"
        "Initialize the submodule (git submodule update --init third_party/bronze), "
        "clone it as a sibling of bro (../bronze), or pass -DBRONZE_DIR=<path>.")
endif()
get_filename_component(_bronze_submodule "${bro_SOURCE_DIR}/third_party/bronze" ABSOLUTE)
get_filename_component(_bronze_chosen "${BRONZE_DIR}" ABSOLUTE)
if(_bronze_chosen STREQUAL _bronze_submodule)
    message(STATUS "bronze: submodule tree (${BRONZE_DIR})")
else()
    message(STATUS "bronze: standalone tree (${BRONZE_DIR})")
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
# The binary directory is bronze-build and NOT bronze, which is not cosmetic:
# bro sends every executable to ${CMAKE_BINARY_DIR} (top-level CMakeLists.txt),
# so the CLI wants to be the file build/bronze — and a directory of that name
# is already there. On Windows the exe is bronze.exe and the two never meet;
# on Linux and macOS the link fails with "cannot open output file: Is a
# directory", pointing at ld rather than at the name clash that caused it.
add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze-build" EXCLUDE_FROM_ALL)

# One name for "build the compiler", on every generator. `--target bronze`
# works under Ninja and Make and CANNOT work under Visual Studio: the targets
# of an EXCLUDE_FROM_ALL subdirectory are left out of the .sln entirely, and
# MSBuild then reports the honest but unhelpful "Project file does not exist:
# bronze.vcxproj". A target declared HERE is in the solution, and naming
# bronze as its dependency is what pulls bronze.vcxproj in with it — the same
# mechanism that already puts bronze_runtime_shared there, via bro_bronze_host.
#
# Guarded because the compiler is optional: -DBRONZE_WITH_LLVM=OFF is a tree
# that runs compiled apps without building them, and there is no `bronze`
# target in it to depend on.
if(TARGET bronze)
    add_custom_target(bronze-cli COMMENT "bronze compiler")
    add_dependencies(bronze-cli bronze)
endif()

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
    host_canvas2d.cpp
    dom_storage.cpp
    dom_gamepad.cpp
    host_element.cpp
    host_element_style.cpp
    host_element_dataset.cpp
    host_element_forms.cpp
    host_element_image.cpp
    host_node.cpp
    host_events.cpp
    host_dom_events.cpp
    host_timers.cpp
    host_image.cpp
    host_codecs.cpp
    host_interp.cpp
    host_platform.cpp
    host_xhr.cpp
    host_fetch.cpp
    host_file.cpp
    host_abort.cpp
    host_observers.cpp
    host_parser.cpp
    host_proxy.cpp
    host_class.cpp
    host_video.cpp
    host_audio_core.cpp
    host_audio_param.cpp
    host_audio_buffer.cpp
    host_audio_nodes.cpp
    host_audio_spatial.cpp
    host_audio_dsp.cpp
    host_physics_core.cpp
    host_physics_constraints.cpp
    host_physics_character.cpp
    host_physics_softbody.cpp
    host_physics_queries.cpp
    host_ai_core.cpp
    host_ai_navgrid.cpp
    host_ai_navmesh.cpp
    host_ai_agent.cpp
    host_net.cpp
    host_vendor_globals.cpp
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
    # ELF and Mach-O need to be TOLD that "beside the executable" is a place to
    # look; only the PE loader searches there by itself. Without this the copy
    # above is enough in the build tree (CMake adds the library's build
    # directory to the rpath) and useless the moment the tree is packaged and
    # moved to another machine — which is precisely what the nightly zip is.
    # APPEND to both rpath properties rather than setting one: the build-tree
    # rpath CMake computes stays, and the relative entry rides along with it.
    if(NOT WIN32)
        if(APPLE)
            set(_bronze_rpath "@loader_path")
        else()
            set(_bronze_rpath "$ORIGIN")
        endif()
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${_bronze_rpath}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${_bronze_rpath}")
    endif()
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
    PUBLIC bro_engine bro_net bro_webgl glad broimage::broimage brogameagent bronze::runtime_shared
)

# bromesh backs bro.mesh.decodeDraco/encodeDraco in the compiled realm
# (host_codecs.cpp); it is 3D-optional, so guard rather than assume.
if(TARGET bromesh)
    target_link_libraries(bro_bronze_host PUBLIC bromesh)
endif()

if(TARGET libcurl_static)
    target_link_libraries(bro_bronze_host PRIVATE libcurl_static)
    target_compile_definitions(bro_bronze_host PRIVATE BRO_HAVE_CURL)
endif()

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
