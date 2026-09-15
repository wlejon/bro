# bronze host layer — always configured (bronze is the JS runtime for bro).

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

# Resolve brass: sibling working tree, else submodule
if(NOT TARGET brass)
    bro_default_sibling_dir(brass _brass_default)
    if(NOT EXISTS "${_brass_default}/include/brass/brass.hpp")
        set(_brass_default "${bro_SOURCE_DIR}/third_party/brass")
    endif()
    if(NOT DEFINED BRASS_ROOT OR BRASS_ROOT STREQUAL "")
        set(BRASS_ROOT "${_brass_default}" CACHE PATH "Path to Brass root directory" FORCE)
    endif()
    get_filename_component(_brass_submodule "${bro_SOURCE_DIR}/third_party/brass" ABSOLUTE)
    get_filename_component(_brass_chosen "${BRASS_ROOT}" ABSOLUTE)
    if(_brass_chosen STREQUAL _brass_submodule)
        message(STATUS "brass: submodule tree (${BRASS_ROOT})")
    else()
        message(STATUS "brass: standalone tree (${BRASS_ROOT})")
    endif()
    add_subdirectory("${BRASS_ROOT}" "${CMAKE_BINARY_DIR}/brass-build" EXCLUDE_FROM_ALL)
endif()

# bronze builds its runtime as a shared library only when asked, and its own
# default for the question is "am I the top-level project" — which is OFF here,
# where it is a subdirectory. bro always needs it ON: an app arrives as a
# module bro LOADS, and host and module must bind to one runtime image. FORCE
# rather than a default, because there is no working configuration of bro with
# it off, so an inherited OFF in a cache would be a link error a long way from
# its cause.
set(BRONZE_BUILD_SHARED_RUNTIME ON CACHE BOOL "" FORCE)

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
if(NOT TARGET bronze_runtime_shared)
    add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze-build" EXCLUDE_FROM_ALL)
endif()

# One name for "build the compiler", on every generator. `--target bronze`
# works under Ninja and Make and CANNOT work under Visual Studio: the targets
# of an EXCLUDE_FROM_ALL subdirectory are left out of the .sln entirely, and
# MSBuild then reports the honest but unhelpful "Project file does not exist:
# bronze.vcxproj". A target declared HERE is in the solution, and naming
# bronze as its dependency is what pulls bronze.vcxproj in with it — the same
# mechanism that already puts bronze_runtime_shared there, via bro_bronze_host.
if(TARGET bronze AND NOT TARGET bronze-cli)
    add_custom_target(bronze-cli COMMENT "bronze compiler")
    add_dependencies(bronze-cli bronze)
endif()

# --- The binding library ---
# bro's own bronze-compiled JavaScript is NOT in it: that is bro_bronze_js,
# defined below the library, because one of the compiled modules is built
# against a manifest a tool linking THIS library prints.
set(BRO_BRONZE_HOST_SOURCES
    gl_constants.cpp
    gl_state.cpp
    gl_buffers.cpp
    gl_shaders.cpp
    gl_textures.cpp
    gl_framebuffers.cpp
    gl_queries.cpp
    gl_transform_feedback.cpp
    gl_context.cpp
    gl_profile.cpp
    host_callee_namer.cpp
    host_telemetry.cpp
    app_module.cpp
    dom_globals.cpp
    host_gc.cpp
    dom_canvas.cpp
    dom_document.cpp
    host_canvas2d.cpp
    host_canvas_gradient.cpp
    dom_storage.cpp
    dom_gamepad.cpp
    host_element.cpp
    host_element_mutate.cpp
    host_element_style.cpp
    host_element_dataset.cpp
    host_element_forms.cpp
    host_element_image.cpp
    host_element_interaction.cpp
    host_html_interfaces.cpp
    host_iframe.cpp
    host_template.cpp
    host_matchmedia.cpp
    host_node.cpp
    host_shadow_dom.cpp
    host_range.cpp
    host_selection.cpp
    host_events.cpp
    host_dom_events.cpp
    host_timers.cpp
    host_image.cpp
    host_platform.cpp
    host_parser.cpp
    host_proxy.cpp
    host_class.cpp
    host_vendor_globals.cpp
    host_anchor_download.cpp
    host_intl.cpp
    host_web_animations.cpp
    host_brokit.cpp
    host_headless.cpp
    host_headless_input.cpp
    host_headless_frame.cpp
    host_headless_test_hooks.cpp
    host_touch.cpp
    eval.cpp
    eval_jit.cpp
    host_noise.cpp
    host_custom_elements.cpp
    host_imagebitmap.cpp
    host_worker_msg.cpp
    host_worker.cpp
    host_window_open.cpp
    host_realm_scope.cpp
    host_observer_hooks.cpp
    host_js_modules.cpp
    host_bro_root.cpp
    native_time.cpp
    native_paths.cpp
    native_window.cpp
    native_settings.cpp
    native_dunder_bro.cpp
    native_mesh.cpp
    native_net.cpp
    natives/net/native_net_register.cpp
    native_rigging.cpp
    natives/rigging/native_rigging_register.cpp
    native_physics_core.cpp
    native_physics_queries.cpp
    native_physics_character.cpp
    native_physics_softbody.cpp
    native_physics_constraints.cpp
    native_physics_vehicle.cpp
    native_physics_ragdoll.cpp
    natives/physics/native_physics_register.cpp
    native_scene_nodes.cpp
    native_scene_lights.cpp
    native_scene_core.cpp
    native_scene_env.cpp
    native_scene_terrain.cpp
    native_scene_clipmap.cpp
    native_scene_tile.cpp
    native_scene_lighting.cpp
    native_scene_anim.cpp
    native_scene_clip.cpp
    native_scene_gizmo.cpp
    natives/scene/native_scene_register.cpp
    natives/terrain/native_terrain_register.cpp
    natives/clipmap/native_clipmap_register.cpp
    natives/tile_world/native_tile_world_register.cpp
    natives/lighting/native_lighting_register.cpp
    natives/animation/native_animation_register.cpp
    natives/gizmo/native_gizmo_register.cpp
    natives/time/native_time_register.cpp
    natives/paths/native_paths_register.cpp
    natives/window/native_window_register.cpp
    natives/settings/native_settings_register.cpp
    natives/dunder_bro/native_dunder_bro_register.cpp
    native_lm_classes.cpp
    native_lm_ops.cpp
    natives/lm/native_lm_register.cpp
    native_rave.cpp
    natives/rave/native_rave_register.cpp
    native_motion.cpp
    natives/motion/native_motion_register.cpp
    native_mic.cpp
    natives/mic/native_mic_register.cpp
    native_sense.cpp
    natives/sense/native_sense_register.cpp
    native_gesture.cpp
    natives/gesture/native_gesture_register.cpp
    native_wake.cpp
    natives/wake/native_wake_register.cpp
    native_kws.cpp
    natives/kws/native_kws_register.cpp
    native_listen.cpp
    natives/listen/native_listen_register.cpp
    native_triposplat.cpp
    natives/triposplat/native_triposplat_register.cpp
    native_diffusion.cpp
    natives/diffusion/native_diffusion_register.cpp
    native_vision.cpp
    natives/vision/native_vision_register.cpp
    native_diar.cpp
    natives/diar/native_diar_register.cpp
    native_stt.cpp
    natives/stt/native_stt_register.cpp
    native_tts.cpp
    natives/tts/native_tts_register.cpp
    host_audio_core.cpp
    host_audio_context.cpp
    host_audio_clips.cpp
    host_audio_voice.cpp
    host_audio_buses.cpp
    host_audio_synth.cpp
    host_audio_sequencer.cpp
    host_audio_param.cpp
    host_audio_buffer.cpp
    host_audio_nodes.cpp
    host_audio_spatial.cpp
    host_audio_dsp.cpp
    host_ai_core.cpp
    host_ai_navgrid.cpp
    host_ai_navmesh.cpp
    host_ai_agent.cpp
    host_ai_unit.cpp
    host_ai_extras.cpp
    host_ai_mcts.cpp
    host_ai_game.cpp
    host_scene_agent.cpp
    host_math_classes.cpp
    host_math_funcs.cpp
    host_text.cpp
    host_steam.cpp
    host_menu.cpp
    host_video.cpp
    host_media.cpp
    host_codecs.cpp
)

add_library(bro_bronze_host STATIC ${BRO_BRONZE_HOST_SOURCES})
target_include_directories(bro_bronze_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/time
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/paths
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/window
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/settings
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/dunder_bro
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/mesh
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/net
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/rigging
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/physics
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/scene
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/terrain
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/clipmap
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/tile_world
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/lighting
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/animation
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/gizmo
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/lm
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/triposplat
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/diffusion
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/vision
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/diar
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/stt
    ${CMAKE_CURRENT_SOURCE_DIR}/natives/tts
)

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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_LINKER_FILE:bronze_runtime_shared> $<TARGET_FILE_DIR:${target}>
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
# `brass_gc_write_barrier` is defined twice in a process that both compiles
# and runs bronze code: by brass's runtime (brass.lib, whose GC object the
# compiler backend always pulls — its exception path references the GC
# assembly stubs) and by bronze's shared runtime (a no-op export for the
# modules it loads). Nothing static referenced the name until brokit_api
# joined the link: its bronze-compiled polyfills call it the way a module
# would. MSVC then resolves that reference against the first library it
# reaches AFTER the referencing object, finds the DLL's import thunk, and
# later loads brass's object for other symbols — LNK2005 on the pair.
#
# Two things make the resolution deterministic instead. The executables
# name brass.lib as their FIRST direct dependency (src/headless, src/app,
# src/server, via BRO_BRASS_LIB — naming it on this target is not enough,
# CMake orders a link line by dependency and brass.lib is also an interface
# dependency of the backend, so from here it lands after the runtime), and
# /INCLUDE names a symbol only brass's GC object defines, so the linker loads
# that object from position one before any static reference to the barrier
# exists. The DLL's member is never pulled and the two never meet. Both
# copies do nothing in bro (brass's generational GC is never active here),
# so which one static code calls does not matter; only that it is one.
# ELF and Mach-O are not affected: a definition in the executable simply
# interposes the shared one.
if(TARGET brass)
    set(BRO_BRASS_LIB brass CACHE INTERNAL "brass static library, linked first by bronze-hosting executables")
else()
    get_target_property(_codegen_brass_libs bronze_codegen_brass INTERFACE_LINK_LIBRARIES)
    set(BRO_BRASS_LIB "" CACHE INTERNAL "brass static library, linked first by bronze-hosting executables")
    foreach(_lib IN LISTS _codegen_brass_libs)
        if(_lib MATCHES "brass\\.(lib|a)$")
            set(BRO_BRASS_LIB "${_lib}" CACHE INTERNAL "brass static library, linked first by bronze-hosting executables")
        endif()
    endforeach()
endif()
if(MSVC)
    target_link_options(bro_bronze_host INTERFACE "/INCLUDE:brass_gc_card_table_base")
endif()

target_link_libraries(bro_bronze_host
    PUBLIC bro_engine bro_webgl glad broimage::broimage bronze::runtime_shared bronze::cli brogameagent
)

# brokit_api supplies the Node half (require(), fs, path, os, child_process,
# process) and the web half (fetch, URL, Blob, encoding, AbortController,
# WebSocket, streams, crypto, indexedDB, ...) of the realm; host_brokit.cpp
# installs both and says which installers it skips in favour of bro's own.
target_link_libraries(bro_bronze_host PUBLIC brokit_api)

if(BRO_WITH_WEBP)
    target_link_libraries(bro_bronze_host PUBLIC bro_webp)
endif()

bro_apply_feature_defs(bro_bronze_host)

# --- bro's own JavaScript, compiled by bronze at build time: bro_bronze_js ---
# Each js/<name>.js is compiled to an object with its own entry symbol, the
# way brokit ships its polyfills (../brokit/src/api/CMakeLists.txt);
# host_js_modules.cpp enters each one through bronze::embed::runEntry while
# the host globals are installed. The objects make up bro_bronze_js, a
# static library every executable links AFTER bro_bronze_host.
#
# WHY A SECOND LIBRARY. bro_core.js is compiled against the NATIVE MANIFEST
# (`bronze build --native-manifest`): the list of `__bro_native.*` entry
# points and signatures that turns its `__bro_native.time.scale` into a
# direct call. That list is printed by bro-native-manifest, a tool that
# links bro_bronze_host and calls the same registerBroNatives bro calls at
# run time (native_manifest_tool.cpp) — so the manifest is an output of the
# host, never a file a person keeps in step with the C++ by hand. A library
# that both contained bro_core.o and was linked by the tool that produces
# bro_core.o's input would be a cycle; the split is what breaks it. The tool
# links js_entry_stubs.cpp in place of the objects (it runs no entry).
#
# THE HOST-GLOBALS MANIFEST is js/module.globals, a checked-in list of the
# bare identifiers these modules are allowed to read (globalThis, console,
# and the host objects C++ registers for them). It is NOT the app manifest:
# an app compiles against what `bro-headless <appdir> --print-host-globals`
# prints — the registry of the binary that will run it — and no such list
# exists while that binary is being built. So bro's modules read everything
# else off `globalThis` at the point of use, and the manifest stays tiny.
#
# On Apple Silicon the compiled objects cannot be linked (bronze's code
# generator emits x86_64, a cputype mismatch for an arm64 Mach-O) and the
# native no-op entry points stand in, exactly as brokit's do; no
# bronze-compiled JS runs there at all.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")))
    add_library(bro_bronze_js STATIC js_entry_stubs.cpp)
else()
    set(BRO_JS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${BRO_JS_GEN_DIR}")
    set(BRO_JS_MODULE_GLOBALS "${CMAKE_CURRENT_SOURCE_DIR}/js/module.globals")
    set(BRO_COMPILED_JS_OBJECTS "")

    # The manifest tool. It links the engine and the binding layer (the
    # natives reach the engine only when called, and the tool calls none),
    # so its link line is the executables' own: BRO_BRASS_LIB first, for
    # the reason above bro_bronze_host's link line.
    add_executable(bro-native-manifest native_manifest_tool.cpp js_entry_stubs.cpp)
    target_link_libraries(bro-native-manifest PRIVATE ${BRO_BRASS_LIB} bro_engine bro_bronze_host)
    if(WIN32)
        target_link_libraries(bro-native-manifest PRIVATE ws2_32)
    endif()
    target_include_directories(bro-native-manifest PRIVATE ${bro_SOURCE_DIR}/src)
    bro_bronze_stage_runtime(bro-native-manifest)

    # generated/natives.json: written to a scratch name and copied over only
    # when its content changed, so bro_core.o is recompiled when a native's
    # path or signature moved and not every time the tool relinked.
    set(BRO_NATIVE_MANIFEST "${BRO_JS_GEN_DIR}/natives.json")
    add_custom_command(
        OUTPUT  "${BRO_NATIVE_MANIFEST}"
        COMMAND $<TARGET_FILE:bro-native-manifest> "${BRO_NATIVE_MANIFEST}.new"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BRO_NATIVE_MANIFEST}.new" "${BRO_NATIVE_MANIFEST}"
        DEPENDS bro-native-manifest
        COMMENT "bro-native-manifest -> generated/natives.json"
        VERBATIM
    )

    # bro_compile_js(<name> <entry_symbol> [NATIVE_MANIFEST <file>]):
    # js/<name>.js -> generated/<name>.o, entered through <entry_symbol>. The
    # object depends on the compiler, the source and the manifest(s), so a
    # change to any of them recompiles it.
    function(bro_compile_js JS_FILE ENTRY_SYMBOL)
        cmake_parse_arguments(ARG "" "NATIVE_MANIFEST" "" ${ARGN})
        set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/js/${JS_FILE}.js")
        set(OUTPUT "${BRO_JS_GEN_DIR}/${JS_FILE}.o")
        set(EXTRA_ARGS "")
        set(EXTRA_DEPS "")
        if(ARG_NATIVE_MANIFEST)
            set(EXTRA_ARGS --native-manifest "${ARG_NATIVE_MANIFEST}")
            set(EXTRA_DEPS "${ARG_NATIVE_MANIFEST}")
        endif()
        add_custom_command(
            OUTPUT  "${OUTPUT}"
            COMMAND $<TARGET_FILE:bronze> build "${INPUT}" --emit-obj
                    --host-globals "${BRO_JS_MODULE_GLOBALS}"
                    ${EXTRA_ARGS}
                    --entry-symbol=${ENTRY_SYMBOL} -o "${OUTPUT}"
            DEPENDS bronze "${INPUT}" "${BRO_JS_MODULE_GLOBALS}" ${EXTRA_DEPS}
            COMMENT "bronze compile js/${JS_FILE}.js -> ${JS_FILE}.o (${ENTRY_SYMBOL})"
            VERBATIM
        )
        set_source_files_properties("${OUTPUT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        list(APPEND BRO_COMPILED_JS_OBJECTS "${OUTPUT}")
        set(BRO_COMPILED_JS_OBJECTS "${BRO_COMPILED_JS_OBJECTS}" PARENT_SCOPE)
    endfunction()

    bro_compile_js(observers  bro_observers_main)
    bro_compile_js(net_sync   bro_net_sync_main)
    bro_compile_js(image_gpu  bro_image_gpu_main)
    bro_compile_js(bro_core   bro_core_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(mesh       bro_mesh_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(net        bro_net_main      NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(rigging    bro_rigging_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(physics    bro_physics_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(terrain    bro_terrain_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(clipmap    bro_clipmap_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(tile_world bro_tile_world_main NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(lighting   bro_lighting_main NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(gizmo      bro_gizmo_main    NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(animation  bro_animation_main NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(scene      bro_scene_main    NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(lm         bro_lm_main         NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(rave       bro_rave_main       NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(motion     bro_motion_main     NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(mic        bro_mic_main        NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(sense      bro_sense_main      NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(gesture    bro_gesture_main    NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(wake       bro_wake_main       NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(kws        bro_kws_main        NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(listen     bro_listen_main     NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(triposplat bro_triposplat_main NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(diffusion  bro_diffusion_main  NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(vision     bro_vision_main     NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(diar       bro_diar_main       NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(stt        bro_stt_main        NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")
    bro_compile_js(tts        bro_tts_main        NATIVE_MANIFEST "${BRO_NATIVE_MANIFEST}")

    add_library(bro_bronze_js STATIC ${BRO_COMPILED_JS_OBJECTS})
    # Nothing in it is C++; the archiver still has to be told which one to be.
    set_target_properties(bro_bronze_js PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(bro_bronze_js INTERFACE bronze::runtime_shared)
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
# --host-globals <manifest>`, where the manifest is what `bro-headless <appdir>
# --print-host-globals` prints — the registry of the binary that will run the
# module, not a file kept in this tree — and this tree can produce the
# compiler that does it. src/bronze_host/README.md has the sequence.
