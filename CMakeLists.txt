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
# non-sibling submodules — the bro-* libraries, bronze and brass resolve from
# sibling repos at ../<name> first (see third_party/CMakeLists.txt and
# docs/ecosystem.md), so it's fine if their third_party/<name> dirs are empty.
foreach(_bro_submodule IN ITEMS SDL jolt)
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
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Bound compilation and link concurrency for Ninja to prevent OOM on memory-constrained systems (e.g. 16GB RAM with 32 hardware threads).
if(CMAKE_GENERATOR STREQUAL "Ninja")
    if(NOT DEFINED BRO_COMPILE_JOBS)
        set(BRO_COMPILE_JOBS 4 CACHE STRING "Maximum parallel compile jobs for Ninja (prevents OOM)")
    endif()
    if(NOT DEFINED BRO_LINK_JOBS)
        set(BRO_LINK_JOBS 2 CACHE STRING "Maximum parallel link jobs for Ninja (prevents OOM)")
    endif()
    set_property(GLOBAL PROPERTY JOB_POOLS compile_job_pool=${BRO_COMPILE_JOBS} link_job_pool=${BRO_LINK_JOBS})
    set(CMAKE_JOB_POOL_COMPILE compile_job_pool)
    set(CMAKE_JOB_POOL_LINK link_job_pool)
endif()

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
        # Symbols in the OPTIMISED configurations too. RelWithDebInfo carries
        # them by default and Release did not, which meant the one build that
        # long headless runs actually use produced crash traces of module +
        # rva and nothing else. /Z7 embeds debug info in .obj (sccache-friendly)
        # and /DEBUG below keeps the linker writing a PDB while /OPT:REF
        # /OPT:ICF preserve release link behaviour, which /DEBUG otherwise
        # turns off.
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release>>:/Z7>
    )
    add_link_options(
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    # String literals with UTF-8 bytes > 127 require unsigned char so they
    # don't trip narrowing-conversion errors on platforms where `char` is
    # signed (Apple clang).
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

# Host layer for bronze JavaScript runtime: src/bronze_host wraps
# the engine's WebGL2 context and a minimal DOM as bronze host globals.
# Bronze is mandatory: it is the JS execution engine for bro.
set(BRO_WITH_BRONZE ON CACHE BOOL "bronze runtime (mandatory)" FORCE)

# Helper: check if a sibling library is present (standalone checkout or submodule)
macro(bro_check_sibling _name _tier _out)
    if(${_tier})
        if(EXISTS "${CMAKE_SOURCE_DIR}/../${_name}/CMakeLists.txt" OR
           EXISTS "${CMAKE_SOURCE_DIR}/third_party/${_name}/CMakeLists.txt")
            set(${_out} ON)
        else()
            set(${_out} OFF)
        endif()
    else()
        set(${_out} OFF)
    endif()
endmacro()

set(_d_linux_seat OFF)
set(_d_linux_wl OFF)
set(_d_linux_portal OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_d_linux_seat ${_d_render})
    set(_d_linux_wl ${_d_render})
    set(_d_linux_portal ${_d_render})
endif()

bro_check_sibling(broconf      _d_svc          _d_conf)
bro_check_sibling(brothemes    _d_render       _d_themes)
bro_check_sibling(brokeys      _d_render       _d_keys)
bro_check_sibling(brosearch    _d_render       _d_search)
bro_check_sibling(broapps      _d_render       _d_apps)
bro_check_sibling(brovfs       _d_render       _d_vfs)
bro_check_sibling(brothumb     _d_render       _d_thumb)
bro_check_sibling(broseat      _d_linux_seat   _d_seat)
bro_check_sibling(brocred      _d_render       _d_cred)
bro_check_sibling(brosys       _d_render       _d_sys)
bro_check_sibling(brodisplays  _d_render       _d_displays)
bro_check_sibling(broportal    _d_linux_portal _d_portal)
bro_check_sibling(brocompositor _d_render      _d_compositor)
bro_check_sibling(browl        _d_linux_wl     _d_wl)
bro_check_sibling(broa11y      _d_render       _d_a11y)

# Services (each pulls a vcpkg dependency).
option(BRO_WITH_NET      "Game networking (GameNetworkingSockets / vcpkg)"   ${_d_svc})
option(BRO_WITH_VIDEO    "Video + GIF encode/decode (libvpx/webm / vcpkg)"   ${_d_svc})
option(BRO_WITH_STEAM    "Steam integration (runtime-loaded redistributable)" ${_d_svc})
option(BRO_WITH_CONF     "Settings store with schemas and watchers (broconf)" ${_d_conf})
option(BRO_WITH_THEMES   "Colour schemes + WCAG/APCA contrast (brothemes)"   ${_d_themes})
option(BRO_WITH_KEYS     "Keybinding engine (brokeys)"                      ${_d_keys})
option(BRO_WITH_SEARCH   "Fuzzy matching, file walking, and grep (brosearch)" ${_d_search})
option(BRO_WITH_APPS     "App catalog, launch, and desktop entries (broapps)" ${_d_apps})
option(BRO_WITH_VFS      "Virtual filesystem and file operations (brovfs)"   ${_d_vfs})
option(BRO_WITH_THUMB    "Freedesktop thumbnailer and cache (brothumb)"       ${_d_thumb})
option(BRO_WITH_SEAT     "Session state, VT switching, idle inhibition (broseat)" ${_d_seat})
option(BRO_WITH_CRED     "Lock screen auth, biometrics, secrets, polkit (brocred)" ${_d_cred})
option(BRO_WITH_SYS      "System services: power, audio, network, bluetooth, tray (brosys)" ${_d_sys})
option(BRO_WITH_DISPLAYS "Display configuration, modes, night light, brightness (brodisplays)" ${_d_displays})
option(BRO_WITH_PORTAL   "Desktop portal backend handlers (broportal)"                      ${_d_portal})
option(BRO_WITH_COMPOSITOR "Window-management policy and compositor (brocompositor)" ${_d_compositor})
option(BRO_WITH_WL         "Wayland client protocol layers and surfaces (browl)"         ${_d_wl})
option(BRO_WITH_A11Y       "Accessibility tree export and announcements (broa11y)"       ${_d_a11y})
# The native <terminal> element: bropty (VT emulation + PTY/ConPTY), its
# brosearch dependency and brothemes (minimum contrast), plus bromux
# (persistent sessions) when found; all vcpkg-free siblings. On in app and
# full; minimal leaves it out (a compiled-out build gets the `available: false` stub).
option(BRO_WITH_TERMINAL "Native <terminal> element (bropty + brosearch)"    ${_d_render})
# Tier 2 — the AI tower (brotensor is the base).
option(BRO_WITH_TENSOR       "brotensor tensor ops (ML backbone)"            ${_d_ai})
option(BRO_WITH_TENSOR_CUDA  "brotensor CUDA backend (needs CUDA toolkit)"   OFF)
option(BRO_WITH_TENSOR_METAL "brotensor Metal backend (macOS)"              OFF)
# BRO_WITH_TENSOR_VULKAN is declared below, after its auto-detection.
option(BRO_WITH_LM           "Language models (brolm)"                       ${_d_ai})
option(BRO_WITH_DIFFUSION    "Diffusion models (brodiffusion)"               ${_d_ai})
option(BRO_WITH_VISION       "Vision models (brovisionml)"                   ${_d_ai})
option(BRO_WITH_SOUNDML      "Audio ML: stt/tts/diar/wake/kws (brosoundml)"  ${_d_ai})
option(BRO_WITH_TRIPOSPLAT   "Image -> 3D gaussian-splat pipeline"           ${_d_ai})
option(BRO_WITH_GAMEAI_NN    "brogameagent neural/learning layer"            ${_d_ai})

# brotensor's cmake helper (BrotensorVulkan.cmake) for the auto-detection
# below, from the brotensor bro will add.
set(_bro_brotensor_cmake_dir "")
foreach(_d "${BROTENSOR_DIR}" "${CMAKE_SOURCE_DIR}/../brotensor" "${CMAKE_SOURCE_DIR}/third_party/brotensor")
    if(_d AND EXISTS "${_d}/cmake/BrotensorVulkan.cmake")
        set(_bro_brotensor_cmake_dir "${_d}/cmake")
        break()
    endif()
endforeach()

# The Vulkan compute backend (brotensor docs/vulkan.md: hand-written GLSL on any
# Vulkan 1.2+ driver), the AMD GPU path. Unlike CUDA / Metal it is additive:
# it can sit beside CUDA, which then stays the default device (CUDA > Metal >
# Vulkan > CPU). Auto-detected on Linux, the first time this build directory
# is configured, when glslc, the Vulkan headers and the Vulkan loader are all
# present (brotensor_vulkan_detect);
# -DBRO_WITH_TENSOR_VULKAN=ON/OFF decides it explicitly.
set(_bro_vulkan_default OFF)
if(NOT DEFINED CACHE{BRO_WITH_TENSOR_VULKAN} AND BRO_WITH_TENSOR AND NOT BRO_WITH_TENSOR_METAL
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _bro_brotensor_cmake_dir)
    include("${_bro_brotensor_cmake_dir}/BrotensorVulkan.cmake")
    brotensor_vulkan_detect(_bro_vulkan_ok)
    if(_bro_vulkan_ok)
        message(STATUS "bro: glslc, Vulkan headers and loader found; enabling BRO_WITH_TENSOR_VULKAN")
        set(_bro_vulkan_default ON)
    endif()
endif()
option(BRO_WITH_TENSOR_VULKAN "brotensor Vulkan backend (glslc + Vulkan headers)" ${_bro_vulkan_default})

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
_bro_require(BRO_WITH_TENSOR_VULKAN BRO_WITH_TENSOR)
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
    set(BROGAMEAGENT_WITH_METAL OFF CACHE BOOL "" FORCE)
elseif(BRO_WITH_TENSOR_METAL)
    set(BROTENSOR_WITH_CUDA    OFF CACHE BOOL "" FORCE)
    set(BROTENSOR_WITH_METAL   ON  CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_CUDA OFF CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_METAL ON CACHE BOOL "" FORCE)
else()
    set(BROTENSOR_WITH_CUDA    OFF CACHE BOOL "" FORCE)
    set(BROTENSOR_WITH_METAL   OFF CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_CUDA OFF CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_METAL OFF CACHE BOOL "" FORCE)
endif()
# Vulkan sits beside whichever backend the chain above picked (on an AMD
# machine, none: Vulkan is the only GPU backend). brodiffusion and brovisionml
# read BROTENSOR_WITH_VULKAN for their GLSL kernels; brolm and brosoundml need
# none.
if(BRO_WITH_TENSOR_VULKAN)
    set(BROTENSOR_WITH_VULKAN     ON  CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_VULKAN  ON  CACHE BOOL "" FORCE)
else()
    set(BROTENSOR_WITH_VULKAN     OFF CACHE BOOL "" FORCE)
    set(BROGAMEAGENT_WITH_VULKAN  OFF CACHE BOOL "" FORCE)
endif()

# Forward Bronze API library enable flags to sibling libraries
set(BROAUDIO_ENABLE_API     ON CACHE BOOL "Build Bronze JavaScript API for broaudio" FORCE)
set(BROGAMEAGENT_ENABLE_API ON CACHE BOOL "Build Bronze JavaScript API for brogameagent" FORCE)
set(BROFLORA_ENABLE_API     ON CACHE BOOL "Build Bronze JavaScript API for broflora" FORCE)
set(BROTENSOR_ENABLE_API    ON CACHE BOOL "Build Bronze JavaScript API for brotensor" FORCE)
set(BROLM_ENABLE_API        ON CACHE BOOL "Build Bronze JavaScript API for brolm" FORCE)
set(BRODIFFUSION_ENABLE_API ON CACHE BOOL "Build Bronze JavaScript API for brodiffusion" FORCE)
set(BROSOUNDML_ENABLE_API   ON CACHE BOOL "Build Bronze JavaScript API for brosoundml" FORCE)
set(BROMESH_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for bromesh" FORCE)
set(BROVISIONML_ENABLE_API  ON CACHE BOOL "Build Bronze JavaScript API for brovisionml" FORCE)
set(BROCONF_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for broconf" FORCE)
set(BROTHEMES_ENABLE_API    ON CACHE BOOL "Build Bronze JavaScript API for brothemes" FORCE)
set(BROKEYS_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for brokeys" FORCE)
set(BROSEARCH_ENABLE_API    ON CACHE BOOL "Build Bronze JavaScript API for brosearch" FORCE)
set(BROTHUMB_ENABLE_API     ON CACHE BOOL "Build Bronze JavaScript API for brothumb" FORCE)
set(BROSEAT_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for broseat" FORCE)
set(BROCRED_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for brocred" FORCE)
set(BROSYS_ENABLE_API       ON CACHE BOOL "Build Bronze JavaScript API for brosys" FORCE)
set(BROIMAGE_ENABLE_API     ON CACHE BOOL "Build Bronze JavaScript API for broimage" FORCE)
set(BROAPPS_ENABLE_API      ON CACHE BOOL "Build Bronze JavaScript API for broapps" FORCE)
set(BROVFS_ENABLE_API       ON CACHE BOOL "Build Bronze JavaScript API for brovfs" FORCE)
set(BROPORTAL_ENABLE_API    ON CACHE BOOL "Build Bronze JavaScript API for broportal" FORCE)
set(BRODISPLAYS_ENABLE_API  ON CACHE BOOL "Build Bronze JavaScript API for brodisplays" FORCE)
set(BROCOMPOSITOR_ENABLE_API ON CACHE BOOL "Build Bronze JavaScript API for brocompositor" FORCE)
set(BROWL_ENABLE_API         ON CACHE BOOL "Build Bronze JavaScript API for browl" FORCE)
set(BROA11Y_ENABLE_API       ON CACHE BOOL "Build Bronze JavaScript API for broa11y" FORCE)

# Stamp every feature flag as a 0/1 compile definition so C++ can `#if` on it.
# Applied to the targets that carry feature-gated code (bro_engine, bro_scene,
# bro_bronze_host). Uses the #if BRO_WITH_X convention.
set(BRO_FEATURE_FLAGS
    3D PHYSICS AUDIO GAMEAI FLORA
    NET VIDEO STEAM TERMINAL CONF THEMES KEYS SEARCH APPS VFS THUMB SEAT CRED SYS DISPLAYS PORTAL COMPOSITOR WL A11Y
    TENSOR TENSOR_CUDA TENSOR_METAL TENSOR_VULKAN LM DIFFUSION VISION SOUNDML TRIPOSPLAT GAMEAI_NN
    BRONZE)
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

# ─── glslang (in-process GLSL -> SPIR-V, build time and run time) ────────────
include(cmake/glslang.cmake)

add_subdirectory(src)
