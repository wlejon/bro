# --- bro's dependencies ---
#
# Every library below comes through bro_dependency() (cmake/bro_deps.cmake),
# pinned in cmake/bro_pins.cmake: a working tree at ../<name> beside bro when
# there is one, otherwise the pinned commit fetched from GitHub at configure.
# There are no submodules. -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> builds any
# of them from another tree. Order matters only where noted: a library a later
# one depends on is added first, so the later one's own lookup finds the target.

# --- FastNoise2 (SIMD noise generation) ---
# Used directly by bro's terrain system, and by brokit, whose own lookup finds
# the target added here.
if(NOT TARGET FastNoise)
    bro_dependency(FastNoise2 TARGET FastNoise SOURCE_ONLY)
    # FastNoise2 fetches FastSIMD through the CPM.cmake copy vendored next to it.
    # CPM caches CPM_DIRECTORY and early-returns (defining nothing) when a
    # *different* copy of CPM.cmake is included later, so a build tree that
    # moved between FastNoise2 trees keeps a stale CPM_DIRECTORY that leaves
    # CPMAddPackage undefined. Drop it when it no longer matches.
    get_filename_component(_fn2_cpm_dir "${FastNoise2_SOURCE_DIR}/cmake" REALPATH)
    if(DEFINED CPM_DIRECTORY AND NOT CPM_DIRECTORY STREQUAL _fn2_cpm_dir)
        unset(CPM_DIRECTORY CACHE)
        unset(CPM_FILE CACHE)
        unset(CPM_VERSION CACHE)
    endif()
    bro_dependency(FastNoise2 TARGET FastNoise
                   OPTIONS FASTNOISE2_NOISETOOL=OFF FASTNOISE2_TESTS=OFF)

    # Workaround: FastNoise2's CMake sets CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY to
    # pdb-files/ without a per-config subdir. The VS generator emits
    # <ProgramDataBaseFileName>pdb-files\Debug</ProgramDataBaseFileName> (no .pdb)
    # for FastSIMD, while the FastNoise target creates an actual pdb-files\Debug\
    # directory. The Copy PDB task then sees a directory at the path it expects
    # to be a file and fails with MSB3025. Give each target a per-config
    # subdir + explicit name so the basenames don't collide.
    foreach(_fn_target FastNoise FastSIMD FastSIMD_FastNoise)
        if(TARGET ${_fn_target})
            set_target_properties(${_fn_target} PROPERTIES
                COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/FastNoise2/pdb-files/$<CONFIG>"
                COMPILE_PDB_NAME ${_fn_target})
        endif()
    endforeach()
endif()

# --- bromath (header-only math library) ---
bro_dependency(bromath)

# --- brass (bronze's code generator) ---
# This is the only place brass is resolved inside bro: bronze's and the
# siblings' own lookups find the target added here. The BRASS_ROOT environment
# variable still names a tree to build it from; BRASS_ROOT in the cache reports
# the tree chosen, which bronze's shared runtime reads for brass's headers;
# set it with -DBRASS_ROOT=<path> to choose another (bronze reads it the same way).
if(NOT TARGET brass)
    if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
        set(FETCHCONTENT_SOURCE_DIR_BRASS "$ENV{BRASS_ROOT}")
    elseif(BRASS_ROOT AND NOT BRASS_ROOT STREQUAL _BRONZE_BRASS_ROOT_CHOSEN)
        set(FETCHCONTENT_SOURCE_DIR_BRASS "${BRASS_ROOT}")
    endif()
    bro_dependency(brass)
    set(BRASS_ROOT "${brass_SOURCE_DIR}" CACHE PATH "brass source tree" FORCE)
    set(_BRONZE_BRASS_ROOT_CHOSEN "${brass_SOURCE_DIR}" CACHE INTERNAL "")
endif()

# --- bronze (the JavaScript compiler and runtime) ---
# Added here, before any sibling, because every sibling's bronze binding
# (<name>_api) links the one runtime bro loads apps into.
#
# bronze builds its runtime as a shared library only when asked, and its own
# default for the question is "am I the top-level project", which is OFF here.
# bro always needs it ON: an app arrives as a module bro LOADS, and host and
# module must bind to one runtime image. EXCLUDE_FROM_ALL (as for every
# dependency) keeps bronze's CLI, backend and test binaries out of bro's
# default target; bronze's own configure requires doctest.
bro_dependency(bronze TARGET bronze_runtime_shared
               OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)
# Reported, not read: scripts/package-release.sh and the tests/bronze_host
# scripts take bronze's headers from the tree this names.
set(BRONZE_DIR "${bronze_SOURCE_DIR}" CACHE PATH "bronze source tree (chosen by bro_dependency)" FORCE)

# --- brotensor (tensor + ops, the AI-tower base) ---
# bro is the FIRST loader of brotensor, so its backend is the one the cache
# vars forwarded in the top-level CMakeLists.txt chose, and every later
# sibling finds the target instead of resolving its own.
if(BRO_WITH_TENSOR)
    bro_dependency(brotensor)
endif()

# --- broimage (image decode/encode + composable kernels) ---
# Must be added before brokit, whose bro.image kernels delegate into broimage.
bro_dependency(broimage)

# curl links whatever zstd it can find, and broimage's basisu transcoder
# carries its own decompress-only copy for Zstd-supercompressed KTX2. Both
# export ZSTD_createDStream and friends, so a build where curl found one is a
# build that fails at link with LNK2005 -- every executable, after everything
# has compiled. Whether curl finds one is a property of the machine (a vcpkg
# zstd on PATH), so pick one here, for everyone: basisu's copy is load-bearing,
# curl's only negotiates a Content-Encoding brokit never asks for.
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)

# --- brokit (JS runtime APIs) ---
bro_dependency(brokit)

# --- htmlayout (CSS + layout library) ---
bro_dependency(htmlayout)

# --- SDL3 ---
bro_dependency(SDL TARGET SDL3-static
               OPTIONS SDL_SHARED=OFF SDL_STATIC=ON SDL_TEST_LIBRARY=OFF)

# --- broaudio (audio engine library) ---
bro_dependency(broaudio)

# --- brolink (the agent control channel's, bromux's and broremote's transport) ---
# Local IPC and the ssh proxy, dependency-free. Every profile: the control
# channel (src/platform/control_socket.cpp, bro-ctl) is always built. Added
# here, once, so bromux and broremote find the same target.
bro_dependency(brolink OPTIONS BROLINK_BUILD_TESTS=OFF)

# --- brosearch + brothemes + bropty + bromux (the <terminal> element) ---
# bropty is the VT emulator and PTY/ConPTY layer behind <terminal>; brosearch
# is its regex scrollback search, brothemes its minimum-contrast adjustment,
# bromux the server behind persistent sessions. Only their libraries are
# built: bro's own terminal tests (src/terminal) cover the integration, and the
# pty_child helper they drive is compiled from bropty's source by bro.
if(BRO_WITH_TERMINAL)
    bro_dependency(brosearch OPTIONS BROSEARCH_BUILD_TESTS=OFF BROSEARCH_BUILD_TOOLS=OFF)
    bro_dependency(brothemes OPTIONS BROTHEMES_BUILD_TESTS=OFF BROTHEMES_BUILD_TOOLS=OFF)
    bro_dependency(bropty OPTIONS BROPTY_BUILD_TESTS=OFF BROPTY_BUILD_TOOLS=OFF)
    # Its `bromux` executable (the server the client starts on demand) lands
    # beside bro.
    bro_dependency(bromux OPTIONS BROMUX_BUILD_TESTS=OFF)
    # MSVC links an executable that exports symbols with an import library
    # named after it in its ARCHIVE directory: for the `bromux` server that
    # is <config>/bromux.lib, the static library it links, and the link fails
    # (LNK1149). Name the import library apart.
    if(TARGET bromux_cli AND MSVC)
        set_target_properties(bromux_cli PROPERTIES ARCHIVE_OUTPUT_NAME bromux_cli)
    endif()
endif()

# --- bromesh (mesh generation + manipulation library) ---
# Backs the 3D scene graph (mesh/terrain/tile nodes); broflora forces 3D on.
if(BRO_WITH_3D)
    bro_dependency(bromesh)
endif()

# --- broflora (ecosystem simulation: Makowski et al. "Synthetic Silviculture") ---
if(BRO_WITH_FLORA)
    bro_dependency(broflora)
endif()

# --- brogameagent (game AI: navmesh, pathfinding, steering, behavior trees) ---
# Embedded by value in the 3D scene graph, so 3D forces GAMEAI on.
if(BRO_WITH_GAMEAI)
    bro_dependency(brogameagent)
endif()

# --- The AI tower ---
# brolm before brosoundml (a brosoundml tokenizer links a brolm one) and before
# brodiffusion, which also needs brovisionml (TripoSplat).
if(BRO_WITH_LM)
    bro_dependency(brolm)
endif()
if(BRO_WITH_SOUNDML)
    bro_dependency(brosoundml)
endif()
if(BRO_WITH_VISION)
    bro_dependency(brovisionml)
endif()
if(BRO_WITH_DIFFUSION)
    bro_dependency(brodiffusion)
endif()

# --- Desktop substrate ---
if(BRO_WITH_CONF)
    bro_dependency(broconf)
endif()
if(BRO_WITH_THEMES)
    bro_dependency(brothemes OPTIONS BROTHEMES_BUILD_TESTS=OFF BROTHEMES_BUILD_TOOLS=OFF)
endif()
if(BRO_WITH_KEYS)
    bro_dependency(brokeys OPTIONS BROKEYS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_SEARCH)
    bro_dependency(brosearch OPTIONS BROSEARCH_BUILD_TESTS=OFF BROSEARCH_BUILD_TOOLS=OFF)
endif()
if(BRO_WITH_VFS)
    bro_dependency(brovfs OPTIONS BROVFS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_APPS)
    bro_dependency(broapps OPTIONS BROAPPS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_THUMB)
    bro_dependency(brothumb OPTIONS BROTHUMB_BUILD_TESTS=OFF)
endif()

# brodbus: the shared sd-bus layer of brocred, brosys, broseat, broportal and
# brompris. Linux only, and only with libsystemd, whose absence those libraries
# handle themselves (brocred and brosys build without D-Bus).
set(_bro_wants_brodbus OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   (BRO_WITH_CRED OR BRO_WITH_SYS OR BRO_WITH_SEAT OR BRO_WITH_PORTAL OR BRO_WITH_MPRIS))
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(_bro_libsystemd QUIET libsystemd)
        if(_bro_libsystemd_FOUND)
            set(_bro_wants_brodbus ON)
        endif()
    endif()
endif()
if(_bro_wants_brodbus)
    bro_dependency(brodbus OPTIONS BRODBUS_BUILD_TESTS=OFF)
endif()

if(BRO_WITH_SEAT)
    bro_dependency(broseat OPTIONS BROSEAT_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_CRED)
    bro_dependency(brocred OPTIONS BROCRED_BUILD_TESTS=OFF)
    if(WIN32 AND TARGET brocred AND EXISTS "${CMAKE_BINARY_DIR}/cppwinrt")
        target_include_directories(brocred PRIVATE "${CMAKE_BINARY_DIR}/cppwinrt")
    endif()
endif()
if(BRO_WITH_SYS)
    bro_dependency(brosys OPTIONS BROSYS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_DISPLAYS)
    bro_dependency(brodisplays OPTIONS BRODISPLAYS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_PORTAL)
    bro_dependency(broportal OPTIONS BROPORTAL_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_COMPOSITOR)
    if(EXISTS "$ENV{HOME}/opt/bc-wlr/lib/pkgconfig/wlroots-0.18.pc")
        set(ENV{PKG_CONFIG_PATH} "$ENV{HOME}/opt/bc-wlr/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
    bro_dependency(brocompositor OPTIONS BROCOMPOSITOR_BUILD_TESTS=OFF)
    if(WIN32 AND TARGET brocompositor_win AND EXISTS "${CMAKE_BINARY_DIR}/cppwinrt")
        target_include_directories(brocompositor_win PRIVATE "${CMAKE_BINARY_DIR}/cppwinrt")
    endif()
endif()
if(BRO_WITH_WL)
    bro_dependency(browl OPTIONS BROWL_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_A11Y)
    bro_dependency(broa11y OPTIONS BROA11Y_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_DMABUF)
    bro_dependency(brodmabuf OPTIONS BRODMABUF_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_DECOR)
    bro_dependency(brodecor OPTIONS BRODECOR_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_CLIP)
    bro_dependency(broclip OPTIONS BROCLIP_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_MPRIS)
    bro_dependency(brompris OPTIONS BROMPRIS_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_PULSE)
    bro_dependency(bropulse OPTIONS BROPULSE_BUILD_TESTS=OFF)
endif()
if(BRO_WITH_IME)
    bro_dependency(broime OPTIONS BROIME_BUILD_TESTS=OFF)
endif()

# --- brovideo + broremote (remote session hosting, bro.remote) ---
# brovideo is the hardware video codec layer (VA-API, Media Foundation) that
# broremote streams the composited frame through; on Linux it takes brodmabuf
# from above. broremote's own tools, viewer and tests build in its own tree.
if(BRO_WITH_REMOTE)
    bro_dependency(brovideo OPTIONS BROVIDEO_BUILD_TESTS=OFF)
    bro_dependency(broremote OPTIONS BROREMOTE_BUILD_TESTS=OFF BROREMOTE_BUILD_TOOLS=OFF
                                     BROREMOTE_BUILD_VIEWER=OFF)
endif()

# --- Jolt Physics ---
# Its CMake project lives in Build/, so it is fetched and added by hand.
if(BRO_WITH_PHYSICS AND NOT TARGET Jolt)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)  # match bro's /MD runtime
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)          # don't clobber bro's flags
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)          # avoid -Werror on Jolt internals
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)  # LTO handled by bro's build
    set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
    set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
    set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    bro_dependency(jolt SOURCE_ONLY)
    add_subdirectory("${jolt_SOURCE_DIR}/Build" "${CMAKE_BINARY_DIR}/_deps/jolt-build" EXCLUDE_FROM_ALL)
endif()

# --- GameNetworkingSockets (via vcpkg) ---
# Gated: this is one of only two vcpkg dependencies, so a minimal (net-off)
# build needs no vcpkg for it.
if(BRO_WITH_NET)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(GameNetworkingSockets CONFIG REQUIRED)
endif()

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)

# --- Skia modules: svg, skshaper (+ HarfBuzz/skunicode), skresources ---
#     (compiled from source against pre-built Skia)
include(skia/skia_modules.cmake)
