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

# Enable Objective-C++ at top level on Apple, since enable_language() inside
# a nested add_subdirectory (Metal backends, broaudio's CoreAudio loopback,
# libremidi's CoreMIDI backend, ...) doesn't reliably populate the
# CMAKE_OBJCXX_* variables for other targets in the same configure.
if(APPLE)
    enable_language(OBJCXX)
endif()

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

if(EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json")
    file(COPY "${CMAKE_BINARY_DIR}/compile_commands.json" DESTINATION "${CMAKE_SOURCE_DIR}")
endif()

if(MSVC)
    # Scope to CXX so these don't leak into nvcc-driven CUDA compilation
    # (brogameagent's optional GPU backend). /MP in particular collides with
    # nvcc's single-input-per-output model.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/MP>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    )
else()
    # Embedded JS bundles contain UTF-8 bytes > 127; require unsigned char
    # so string literals don't trip narrowing-conversion errors on platforms
    # where `char` is signed (Apple clang).
    add_compile_options(-funsigned-char)
endif()

# ─── Modular build: profiles + feature flags ────────────────────────────────
# A minimal build needs no vcpkg, no CUDA toolkit, and no brotensor. Profiles
# seed the per-feature defaults; individual -DBRO_WITH_* flags always override.
# See docs/build-options.md for the full rationale and the tier layout.
#
# The default is "app": full renderer (3D + physics + audio + core game-AI) plus
# net/video/steam, but no AI tower (no brotensor/CUDA). "full" adds the AI tower;
# "minimal" is the 2D/canvas/WebGL + audio floor — it links, runs, and renders
# with no vcpkg, no Jolt/bromesh/brogameagent, and no AI tower (the renderer and
# service subsystems are gated behind their BRO_WITH_* flags at the engine seam).
set(BRO_PROFILE "app" CACHE STRING "Feature profile: minimal | app | full")
set_property(CACHE BRO_PROFILE PROPERTY STRINGS minimal app full)

# Per-profile defaults.  minimal = 2D/canvas/WebGL + audio (broaudio is
# self-contained and <audio>/Web Audio is core HTML, so it stays in every
# profile).  app = full renderer (3D + physics + core game-AI) + net/video/
# steam.  full = everything (the CUDA sub-lever stays opt-in even here).
if(BRO_PROFILE STREQUAL "minimal")
    set(_d_render OFF)
    set(_d_ai     OFF)
    set(_d_svc    OFF)
elseif(BRO_PROFILE STREQUAL "app")
    set(_d_render ON)
    set(_d_ai     OFF)
    set(_d_svc    ON)   # net + video + steam: a proper web/app runtime by default
else()  # full
    set(_d_render ON)
    set(_d_ai     ON)
    set(_d_svc    ON)
endif()

# Audio is on in *every* profile, including minimal: broaudio + audio_inference
# are brotensor/vcpkg-free and self-contained, and <audio>/Web Audio is a core
# part of HTML. The flag stays (so a niche audio-less build is still possible),
# but it defaults ON regardless of profile. Video is deliberately NOT symmetric:
# <video> decode needs libvpx (vcpkg), so it lives in the service tier.
set(_d_audio ON)

# Text shaping (HarfBuzz + Skia's UAX#9 bidi subset) is likewise ON in *every*
# profile, minimal included. It is deliberately not a vcpkg dependency: the
# sources are compiled straight out of the Skia source bundle we already ship,
# so `minimal` still configures and builds with no package manager at all. The
# point of that is one text path, not two — a build where "fi" is two glyphs and
# Arabic doesn't join is a different renderer, not a smaller one. The flag exists
# as an escape hatch for a Skia source tree that predates the shaping sources.
set(_d_shaping ON)

# Tier 1 — renderer feature groups (all brotensor-free, all vcpkg-free).
option(BRO_WITH_3D       "3D scene graph (bromesh, terrain, tile, gizmo)"     ${_d_render})
option(BRO_WITH_PHYSICS  "Jolt rigid-body physics"                           ${_d_render})
option(BRO_WITH_AUDIO    "broaudio engine + audio inference"                 ${_d_audio})
option(BRO_WITH_GAMEAI   "brogameagent core (navmesh/pathfinding/MCTS)"      ${_d_render})
option(BRO_WITH_FLORA    "broflora ecosystem simulation"                     ${_d_render})
option(BRO_WITH_TEXT_SHAPING "HarfBuzz text shaping + UAX#9 bidi (Skia modules)" ${_d_shaping})
# Build the stock bro/bro-headless/bro-server executables. A host application
# that embeds the engine (add_subdirectory(bro), link bro_engine, supply its
# own main) turns this OFF: it wants the libraries, not a second bro.exe in
# its build tree. Defaults to ON, and to OFF when bro is not the top-level
# project — an embedder gets the sensible thing without asking for it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_d_exes ON)
else()
    set(_d_exes OFF)
endif()
option(BRO_BUILD_EXECUTABLES "Build the bro/bro-headless/bro-server binaries" ${_d_exes})
# bro's own C++ unit tests and smoke tools (bro_tile_test, bro_videoinspect,
# bro_videoencodetest, bro_mediabackendtest, bro_mediaclocktest). They test
# bro, not the application embedding it, so they follow the same rule as the
# executables: ON for a plain bro build, OFF under add_subdirectory. The JS
# suite in tests/ is unaffected — it runs on bro-headless.
option(BRO_BUILD_TESTS "Build bro's C++ unit tests and smoke tools" ${_d_exes})

# Host layer for bronze-compiled (AOT) JavaScript apps: src/bronze_host wraps
# the engine's WebGL2 context and a minimal DOM as bronze host globals, so a
# compiled three.js app can render through bro. OFF by default and entirely
# outside the default configure path — turning it ON resolves a bronze
# checkout (`../bronze` standalone repo, else `third_party/bronze` submodule) and
# builds its runtime + embed libraries into this tree.
option(BRO_WITH_BRONZE "bronze AOT host layer (resolves ../bronze or third_party/bronze)" OFF)

# Services (each pulls a vcpkg dependency).
option(BRO_WITH_NET      "Game networking (GameNetworkingSockets / vcpkg)"   ${_d_svc})
option(BRO_WITH_VIDEO    "Video + GIF encode/decode (libvpx/webm / vcpkg)"   ${_d_svc})
option(BRO_WITH_STEAM    "Steam integration (runtime-loaded redistributable)" ${_d_svc})
# Tier 2 — the AI tower (brotensor is the base).
option(BRO_WITH_TENSOR       "brotensor tensor ops (ML backbone)"            ${_d_ai})
option(BRO_WITH_TENSOR_CUDA  "brotensor CUDA backend (needs CUDA toolkit)"   OFF)
option(BRO_WITH_TENSOR_METAL "brotensor Metal backend (macOS)"              OFF)
option(BRO_WITH_LM           "Language models (brolm)"                       ${_d_ai})
option(BRO_WITH_DIFFUSION    "Diffusion models (brodiffusion)"               ${_d_ai})
option(BRO_WITH_VISION       "Vision models (brovisionml)"                   ${_d_ai})
option(BRO_WITH_SOUNDML      "Audio ML: stt/tts/diar/wake/kws (brosoundml)"  ${_d_ai})
option(BRO_WITH_TRIPOSPLAT   "Image -> 3D gaussian-splat pipeline"           ${_d_ai})
option(BRO_WITH_GAMEAI_NN    "brogameagent neural/learning layer"            ${_d_ai})

# Auto-enable prerequisites (a feature forces on what it needs). Ordered
# top-down through the dependency DAG so a single pass resolves everything.
function(_bro_require flag dep)
    if(${flag} AND NOT ${dep})
        message(STATUS "bro: ${flag}=ON forces ${dep}=ON")
        set(${dep} ON CACHE BOOL "" FORCE)
    endif()
endfunction()
_bro_require(BRO_WITH_TENSOR_CUDA  BRO_WITH_TENSOR)
_bro_require(BRO_WITH_TENSOR_METAL BRO_WITH_TENSOR)
_bro_require(BRO_WITH_TRIPOSPLAT   BRO_WITH_VISION)
_bro_require(BRO_WITH_TRIPOSPLAT   BRO_WITH_DIFFUSION)
_bro_require(BRO_WITH_TRIPOSPLAT   BRO_WITH_3D)
_bro_require(BRO_WITH_DIFFUSION    BRO_WITH_LM)
_bro_require(BRO_WITH_SOUNDML      BRO_WITH_LM)
_bro_require(BRO_WITH_SOUNDML      BRO_WITH_AUDIO)
_bro_require(BRO_WITH_LM           BRO_WITH_TENSOR)
_bro_require(BRO_WITH_VISION       BRO_WITH_TENSOR)
_bro_require(BRO_WITH_GAMEAI_NN    BRO_WITH_GAMEAI)
_bro_require(BRO_WITH_GAMEAI_NN    BRO_WITH_TENSOR)
_bro_require(BRO_WITH_FLORA        BRO_WITH_3D)
# The 3D scene graph embeds physics + game-AI by value: scene_graph.h includes
# physics_node.h / agent_binding.h / ai_world_ticker.h and bro_scene hard-links
# bro_physics + brogameagent. So enabling 3D forces both on (rather than
# threading #if guards through the whole scene graph). Runs after FLORA→3D so
# the FLORA→3D→{PHYSICS,GAMEAI} chain fully resolves in one pass.
_bro_require(BRO_WITH_3D           BRO_WITH_PHYSICS)
_bro_require(BRO_WITH_3D           BRO_WITH_GAMEAI)

# Forward feature choices into the sibling repos (must happen before
# third_party/ is added — brotensor's backend is fixed at first add_subdirectory,
# and broimage/brogameagent decide their brotensor dependency from these).
if(BRO_WITH_TENSOR)
    set(BROIMAGE_WITH_TENSOR ON  CACHE BOOL "" FORCE)
else()
    set(BROIMAGE_WITH_TENSOR OFF CACHE BOOL "" FORCE)
endif()
if(BRO_WITH_GAMEAI_NN)
    set(BROGAMEAGENT_WITH_NN ON  CACHE BOOL "" FORCE)
else()
    set(BROGAMEAGENT_WITH_NN OFF CACHE BOOL "" FORCE)
endif()
if(BRO_WITH_GAMEAI)
    set(BROGAMEAGENT_WITH_NAVMESH ON CACHE BOOL "" FORCE)
else()
    set(BROGAMEAGENT_WITH_NAVMESH OFF CACHE BOOL "" FORCE)
endif()
if(BRO_WITH_TENSOR_CUDA)
    set(BROTENSOR_WITH_CUDA    ON  CACHE BOOL "" FORCE)
    set(BROTENSOR_WITH_METAL   OFF CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_CUDA ON  CACHE BOOL "" FORCE)
elseif(BRO_WITH_TENSOR_METAL)
    set(BROTENSOR_WITH_CUDA    OFF CACHE BOOL "" FORCE)
    set(BROTENSOR_WITH_METAL   ON  CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_METAL ON CACHE BOOL "" FORCE)
endif()

# Stamp every feature flag as a 0/1 compile definition so C++ can `#if` on it.
# Applied to the targets that carry feature-gated code (bro_js, bro_engine,
# bro_scene). See src/js/*_bindings.cpp for the #if BRO_WITH_X convention.
set(BRO_FEATURE_FLAGS
    3D PHYSICS AUDIO GAMEAI FLORA
    NET VIDEO STEAM
    TENSOR TENSOR_CUDA TENSOR_METAL LM DIFFUSION VISION SOUNDML TRIPOSPLAT GAMEAI_NN)
function(bro_apply_feature_defs target)
    foreach(_f IN LISTS BRO_FEATURE_FLAGS)
        if(BRO_WITH_${_f})
            target_compile_definitions(${target} PUBLIC BRO_WITH_${_f}=1)
        else()
            target_compile_definitions(${target} PUBLIC BRO_WITH_${_f}=0)
        endif()
    endforeach()
endfunction()

add_subdirectory(third_party)

# Stamp the feature flags GLOBALLY on every bro src target (third_party is
# already added above, so the siblings are unaffected). bro_apply_feature_defs
# still stamps the specific targets that carry gated .cpp bodies, but headers
# like engine.h use `#if BRO_WITH_X` to include/exclude *members* — which
# changes a class's layout. Every TU that includes such a header must see the
# same flag value or the layouts disagree (an ODR/ABI mismatch). A global
# definition guarantees that for all bro sources, not just the three stamped
# libs.
foreach(_f IN LISTS BRO_FEATURE_FLAGS)
    if(BRO_WITH_${_f})
        add_compile_definitions(BRO_WITH_${_f}=1)
    else()
        add_compile_definitions(BRO_WITH_${_f}=0)
    endif()
endforeach()

add_subdirectory(src)
