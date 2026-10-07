# --- Where a standalone sibling checkout is looked for ---
#
# Every block below prefers a standalone repo and falls back to the git
# submodule beside this file. `bro_default_sibling_dir()` is what decides where
# "standalone" means, and it checks two roots in order:
#
#   1. beside the top-level project (`${CMAKE_SOURCE_DIR}/..`)
#   2. beside bro itself           (`${bro_SOURCE_DIR}/..`)
#
# (1) is the historical rule and stays first, so nothing about a plain bro build
# or a host application sited as bro's sibling (../ffmpeg-bro, ../planet-bro)
# changes: in the documented layout both roots are the same directory.
#
# (2) exists because a host application's CMake root is not required to be a
# sibling of bro. A standalone compiler/generator may generate its project inside
# the app tree it compiles, so `${CMAKE_SOURCE_DIR}/..` lands somewhere with no
# libraries in it — and the old single-root lookup then fell through to bro's
# *submodules*, silently building a different (and, in a non-recursive clone,
# absent) source tree. The failure surfaced far from its cause, as brokit's
# FastNoise2 submodule missing a CMakeLists.txt.
#
# An explicit `-D<NAME>_DIR=` still wins over both: these only supply the
# default for a cache entry that CMake leaves alone once it is set. Pointing one
# at a nonexistent path (`-DBROKIT_DIR=none`) still forces the submodule.
function(bro_default_sibling_dir _name _out)
    foreach(_root "${CMAKE_SOURCE_DIR}/.." "${bro_SOURCE_DIR}/..")
        get_filename_component(_candidate "${_root}/${_name}" ABSOLUTE)
        if(EXISTS "${_candidate}/CMakeLists.txt")
            set(${_out} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    # Nothing standalone anywhere: hand back the historical path so the caller's
    # own EXISTS test fails and the submodule fallback runs exactly as before.
    get_filename_component(_candidate "${CMAKE_SOURCE_DIR}/../${_name}" ABSOLUTE)
    set(${_out} "${_candidate}" PARENT_SCOPE)
endfunction()



# --- FastNoise2 (SIMD noise generation) ---
# Used directly by bro's terrain system. The source lives inside brokit's
# third_party; brokit guards its own add_subdirectory with if(NOT TARGET FastNoise)
# so this won't conflict.
if(NOT TARGET FastNoise)
    set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "" FORCE)
    set(FASTNOISE2_TESTS OFF CACHE BOOL "" FORCE)
    # Resolve via brokit's tree (standalone or submodule), by the same rule the
    # brokit block below uses. That block runs *after* this one, so BROKIT_DIR
    # only has a value here when the caller passed `-DBROKIT_DIR=`; honour it
    # when they did, and otherwise take the same two-root default.
    if(BROKIT_DIR)
        set(_BROKIT_ROOT "${BROKIT_DIR}")
    else()
        bro_default_sibling_dir(brokit _BROKIT_ROOT)
    endif()
    if(NOT EXISTS "${_BROKIT_ROOT}/CMakeLists.txt")
        set(_BROKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/brokit")
    endif()
    # FastNoise2 fetches FastSIMD through the CPM.cmake copy vendored next to it.
    # CPM caches CPM_DIRECTORY and early-returns (defining nothing) when a *different*
    # copy of CPM.cmake is included later. Switching brokit roots between configures
    # -- submodule to standalone or back -- therefore leaves a stale CPM_DIRECTORY
    # that makes CPMAddPackage undefined. Drop it when it no longer matches.
    get_filename_component(_fn2_cpm_dir
                           "${_BROKIT_ROOT}/third_party/FastNoise2/cmake" REALPATH)
    if(DEFINED CPM_DIRECTORY AND NOT CPM_DIRECTORY STREQUAL _fn2_cpm_dir)
        unset(CPM_DIRECTORY CACHE)
        unset(CPM_FILE CACHE)
        unset(CPM_VERSION CACHE)
    endif()
    add_subdirectory("${_BROKIT_ROOT}/third_party/FastNoise2"
                     "${CMAKE_BINARY_DIR}/FastNoise2" EXCLUDE_FROM_ALL)

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
# Prefer the standalone repo at ../bromath for local development.
# Guard against double-add — sibling libs (bromesh, brogameagent, etc.)
# also pull bromath in. First-loader wins.
if(NOT TARGET bromath)
    bro_default_sibling_dir(bromath _bromath_default)
    set(BROMATH_DIR "${_bromath_default}" CACHE PATH "Path to standalone bromath repo")
    if(EXISTS "${BROMATH_DIR}/CMakeLists.txt")
        message(STATUS "bromath: using standalone repo at ${BROMATH_DIR}")
        add_subdirectory("${BROMATH_DIR}" "${CMAKE_BINARY_DIR}/bromath" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "bromath: using submodule at third_party/bromath")
        add_subdirectory(bromath EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brass (JIT compiler and intermediate representation) ---
# This is the only place brass is resolved inside bro: bronze's and the
# siblings' own brass blocks all sit behind `if(NOT TARGET brass)`. So the knob
# is BRASS_ROOT, the name bronze and CI use — a BRASS_ROOT read anywhere later
# would arrive after the target exists and change nothing. An explicit root
# that holds no brass is an error rather than a quiet fall-through, and the
# chosen tree is always reported, as bronze's is.
if(NOT TARGET brass)
    get_filename_component(_brass_submodule "${CMAKE_CURRENT_SOURCE_DIR}/brass" ABSOLUTE)
    if(DEFINED ENV{BRASS_ROOT} AND NOT "$ENV{BRASS_ROOT}" STREQUAL "")
        set(BRASS_ROOT "$ENV{BRASS_ROOT}" CACHE PATH "brass source tree" FORCE)
    elseif(NOT DEFINED BRASS_ROOT OR BRASS_ROOT STREQUAL "")
        bro_default_sibling_dir(brass _brass_default)
        if(NOT EXISTS "${_brass_default}/CMakeLists.txt")
            set(_brass_default "${_brass_submodule}")
        endif()
        set(BRASS_ROOT "${_brass_default}" CACHE PATH "brass source tree")
    endif()
    if(NOT EXISTS "${BRASS_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "brass not found at\n    ${BRASS_ROOT}\n"
            "Initialize the submodule (git submodule update --init third_party/brass), "
            "clone it as a sibling of bro (../brass), or pass -DBRASS_ROOT=<path>.")
    endif()
    get_filename_component(_brass_chosen "${BRASS_ROOT}" ABSOLUTE)
    if(_brass_chosen STREQUAL _brass_submodule)
        message(STATUS "brass: submodule tree (${_brass_chosen})")
    else()
        message(STATUS "brass: standalone tree (${_brass_chosen})")
    endif()
    add_subdirectory("${_brass_chosen}" "${CMAKE_BINARY_DIR}/brass-build" EXCLUDE_FROM_ALL)
endif()

# --- brotensor (tensor + ops — the AI-tower base) ---
# bro is the FIRST loader of brotensor so that every sibling's own
# `if(NOT TARGET brotensor)` guard (broimage, brogameagent, brolm, brodiffusion,
# brovisionml, brosoundml) trips and none of them run their own resolution. Each
# sibling's fallback assumes it is the standalone CMake root — when nested here
# it resolves `../brotensor` / `third_party/brotensor` against bro's root or its
# own dir and misses the submodule (this is why the full-profile nightly failed
# to configure). Loading it once here, with bro's correct paths, sidesteps all
# of that. Prefer the standalone repo at ../brotensor for local development;
# fall back to the git submodule. Gated on BRO_WITH_TENSOR; the backend cache
# vars (BROTENSOR_WITH_CUDA/METAL) are forwarded in the top-level CMakeLists.txt
# before this file is added, so the backend is fixed at this first load.
if(BRO_WITH_TENSOR AND NOT TARGET brotensor)
    bro_default_sibling_dir(brotensor _brotensor_default)
    set(BROTENSOR_DIR "${_brotensor_default}" CACHE PATH "Path to standalone brotensor repo")
    if(EXISTS "${BROTENSOR_DIR}/CMakeLists.txt")
        message(STATUS "brotensor: using standalone repo at ${BROTENSOR_DIR}")
        add_subdirectory("${BROTENSOR_DIR}" "${CMAKE_BINARY_DIR}/brotensor" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brotensor: using submodule at third_party/brotensor")
        add_subdirectory(brotensor EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broimage (image decode/encode + composable kernels) ---
# Single home for image work that used to be duplicated across the stack.
# Depends on bromath (header-only, added above) and brotensor (CPU + optional
# CUDA/Metal). brotensor's backend is decided at first add_subdirectory(), so
# this block is the first loader of brotensor; later siblings reuse the same
# target via if(NOT TARGET brotensor) guards. The GPU cache vars are forwarded
# in bro's top-level CMakeLists.txt before this point, so the backend is
# already set when brotensor loads here.
#
# Must be added before brokit, because brokit's bro.image kernels delegate
# into broimage.
bro_default_sibling_dir(broimage _broimage_default)
set(BROIMAGE_DIR "${_broimage_default}" CACHE PATH "Path to standalone broimage repo")
if(EXISTS "${BROIMAGE_DIR}/CMakeLists.txt")
    message(STATUS "broimage: using standalone repo at ${BROIMAGE_DIR}")
    add_subdirectory("${BROIMAGE_DIR}" "${CMAKE_BINARY_DIR}/broimage" EXCLUDE_FROM_ALL)
else()
    message(STATUS "broimage: using submodule at third_party/broimage")
    add_subdirectory(broimage EXCLUDE_FROM_ALL)
endif()

# curl links whatever zstd it can find, and broimage's basisu transcoder
# carries its own decompress-only copy for Zstd-supercompressed KTX2. Both
# export ZSTD_createDStream and friends, so a build where curl found one is a
# build that fails at link with LNK2005 -- every executable, after everything
# has compiled.
#
# Whether curl finds one is a property of the machine, not of this repo: CMake
# searches PATH-adjacent prefixes, so a developer who installed vcpkg for the
# default profile has a zstd on PATH and gets the collision even in `minimal`,
# which is documented as needing nothing beyond the submodules. Same source
# tree, same command, different answer per machine.
#
# So pick one, here, for everyone. basisu's copy is load-bearing -- without it
# a texture format stops loading. curl's is Content-Encoding negotiation, and
# brokit already turned zlib off in its own curl configuration, so this build's
# curl does no content decompression either way; zstd was just the one encoding
# nobody thought to switch off, and it happens to be the one that collides.
#
# The durable home for this line is beside that CURL_ZLIB in brokit. It is here
# because this is the tree a first clone gets.
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)

# --- brokit (JS runtime APIs) ---
# Prefer the standalone repo at ../brokit for local development.
# Falls back to the git submodule at third_party/brokit.
bro_default_sibling_dir(brokit _brokit_default)
set(BROKIT_DIR "${_brokit_default}" CACHE PATH "Path to standalone brokit repo")
if(EXISTS "${BROKIT_DIR}/CMakeLists.txt")
    message(STATUS "brokit: using standalone repo at ${BROKIT_DIR}")
    add_subdirectory("${BROKIT_DIR}" "${CMAKE_BINARY_DIR}/brokit" EXCLUDE_FROM_ALL)
else()
    message(STATUS "brokit: using submodule at third_party/brokit")
    add_subdirectory(brokit EXCLUDE_FROM_ALL)
endif()

# --- htmlayout (CSS + layout library) ---
# Prefer the standalone repo at ../htmlayout for local development (edit once, no sync dance).
# Falls back to the git submodule at third_party/htmlayout.
bro_default_sibling_dir(htmlayout _htmlayout_default)
set(HTMLAYOUT_DIR "${_htmlayout_default}" CACHE PATH "Path to standalone htmlayout repo")
if(EXISTS "${HTMLAYOUT_DIR}/CMakeLists.txt")
    message(STATUS "htmlayout: using standalone repo at ${HTMLAYOUT_DIR}")
    add_subdirectory("${HTMLAYOUT_DIR}" "${CMAKE_BINARY_DIR}/htmlayout" EXCLUDE_FROM_ALL)
else()
    message(STATUS "htmlayout: using submodule at third_party/htmlayout")
    add_subdirectory(htmlayout EXCLUDE_FROM_ALL)
endif()

# --- SDL3 (built from submodule) ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL EXCLUDE_FROM_ALL)


# --- broaudio (audio engine library) ---
# Prefer the standalone repo at ../broaudio for local development.
# Falls back to the git submodule at third_party/broaudio.
bro_default_sibling_dir(broaudio _broaudio_default)
set(BROAUDIO_DIR "${_broaudio_default}" CACHE PATH "Path to standalone broaudio repo")
if(EXISTS "${BROAUDIO_DIR}/CMakeLists.txt")
    message(STATUS "broaudio: using standalone repo at ${BROAUDIO_DIR}")
    add_subdirectory("${BROAUDIO_DIR}" "${CMAKE_BINARY_DIR}/broaudio" EXCLUDE_FROM_ALL)
else()
    message(STATUS "broaudio: using submodule at third_party/broaudio")
    add_subdirectory(broaudio EXCLUDE_FROM_ALL)
endif()

# --- brosearch + bropty (the <terminal> element) ---
# bropty is the VT emulator and PTY/ConPTY layer behind <terminal>; brosearch
# is its regex scrollback search. Both prefer the standalone repo at
# ../<name> and fall back to the submodule at third_party/<name>. brosearch is
# added first, so bropty's own `if(NOT TARGET brosearch)` lookup (which
# resolves against the top-level project, not against bro's third_party/)
# finds the target bro already chose. Neither one's tests are built here:
# bro's own terminal tests (src/terminal) cover the integration, and the
# pty_child helper they drive is compiled from bropty's source by bro.
if(BRO_WITH_TERMINAL)
    set(BROSEARCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROSEARCH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BROPTY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROPTY_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    if(NOT TARGET brosearch)
        bro_default_sibling_dir(brosearch _brosearch_default)
        set(BROSEARCH_DIR "${_brosearch_default}" CACHE PATH "Path to standalone brosearch repo")
        if(EXISTS "${BROSEARCH_DIR}/CMakeLists.txt")
            message(STATUS "brosearch: using standalone repo at ${BROSEARCH_DIR}")
            add_subdirectory("${BROSEARCH_DIR}" "${CMAKE_BINARY_DIR}/brosearch" EXCLUDE_FROM_ALL)
        else()
            message(STATUS "brosearch: using submodule at third_party/brosearch")
            add_subdirectory(brosearch EXCLUDE_FROM_ALL)
        endif()
    endif()
    # brothemes: the terminal's minimum-contrast adjustment (WCAG ratio, hue
    # kept in Oklch). Same sibling-first lookup; only its library is built.
    set(BROTHEMES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROTHEMES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    if(NOT TARGET brothemes)
        bro_default_sibling_dir(brothemes _brothemes_default)
        set(BROTHEMES_DIR "${_brothemes_default}" CACHE PATH "Path to standalone brothemes repo")
        if(EXISTS "${BROTHEMES_DIR}/CMakeLists.txt")
            message(STATUS "brothemes: using standalone repo at ${BROTHEMES_DIR}")
            add_subdirectory("${BROTHEMES_DIR}" "${CMAKE_BINARY_DIR}/brothemes" EXCLUDE_FROM_ALL)
        else()
            message(STATUS "brothemes: using submodule at third_party/brothemes")
            add_subdirectory(brothemes EXCLUDE_FROM_ALL)
        endif()
    endif()
    if(NOT TARGET bropty)
        bro_default_sibling_dir(bropty _bropty_default)
        set(BROPTY_DIR "${_bropty_default}" CACHE PATH "Path to standalone bropty repo")
        if(EXISTS "${BROPTY_DIR}/CMakeLists.txt")
            message(STATUS "bropty: using standalone repo at ${BROPTY_DIR}")
            add_subdirectory("${BROPTY_DIR}" "${CMAKE_BINARY_DIR}/bropty" EXCLUDE_FROM_ALL)
        else()
            message(STATUS "bropty: using submodule at third_party/bropty")
            add_subdirectory(bropty EXCLUDE_FROM_ALL)
        endif()
    endif()
    # bromux: persistent terminal sessions (a per-user server that owns the
    # shells; <terminal> elements attach to them and survive reloads). Same
    # sibling-first lookup; it reuses the bropty / brosearch targets above.
    # Its `bromux` executable (the server the client starts on demand) lands
    # beside bro. Without it, persistent sessions are compiled out
    # (BRO_TERMINAL_MUX undefined) and the JS surface says so.
    set(BROMUX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(NOT TARGET bromux)
        bro_default_sibling_dir(bromux _bromux_default)
        set(BROMUX_DIR "${_bromux_default}" CACHE PATH "Path to standalone bromux repo")
        if(EXISTS "${BROMUX_DIR}/CMakeLists.txt")
            message(STATUS "bromux: using standalone repo at ${BROMUX_DIR}")
            # Not ${CMAKE_BINARY_DIR}/bromux: single-config generators put
            # the `bromux` executable there (CMAKE_RUNTIME_OUTPUT_DIRECTORY).
            add_subdirectory("${BROMUX_DIR}" "${CMAKE_BINARY_DIR}/bromux-lib" EXCLUDE_FROM_ALL)
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bromux/CMakeLists.txt")
            message(STATUS "bromux: using submodule at third_party/bromux")
            add_subdirectory(bromux "${CMAKE_BINARY_DIR}/bromux-lib" EXCLUDE_FROM_ALL)
        else()
            message(STATUS "bromux: not found (../bromux or third_party/bromux); persistent terminal sessions off")
        endif()
    endif()
    # MSVC links an executable that exports symbols with an import library
    # named after it in its ARCHIVE directory: for the `bromux` server that
    # is bromux-lib/<config>/bromux.lib, the static library it links, and
    # the link fails (LNK1149). Name the import library apart.
    if(TARGET bromux_cli AND MSVC)
        set_target_properties(bromux_cli PROPERTIES ARCHIVE_OUTPUT_NAME bromux_cli)
    endif()
endif()

# --- bromesh (mesh generation + manipulation library) ---
# Prefer the standalone repo at ../bromesh for local development.
# Falls back to the git submodule at third_party/bromesh.
# Gated on BRO_WITH_3D: bromesh backs the 3D scene graph (mesh/terrain/tile
# nodes), all of which are 3D-only. broflora forces
# 3D on, so its transitive bromesh dependency is always satisfied.
if(BRO_WITH_3D)
    bro_default_sibling_dir(bromesh _bromesh_default)
    set(BROMESH_DIR "${_bromesh_default}" CACHE PATH "Path to standalone bromesh repo")
    if(EXISTS "${BROMESH_DIR}/CMakeLists.txt")
        message(STATUS "bromesh: using standalone repo at ${BROMESH_DIR}")
        add_subdirectory("${BROMESH_DIR}" "${CMAKE_BINARY_DIR}/bromesh" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "bromesh: using submodule at third_party/bromesh")
        add_subdirectory(bromesh EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broflora (ecosystem simulation: Makowski et al. "Synthetic Silviculture") ---
# Prefer the standalone repo at ../broflora for local development.
# Falls back to the git submodule at third_party/broflora.
if(BRO_WITH_FLORA)
    bro_default_sibling_dir(broflora _broflora_default)
    set(BROFLORA_DIR "${_broflora_default}" CACHE PATH "Path to standalone broflora repo")
    if(EXISTS "${BROFLORA_DIR}/CMakeLists.txt")
        message(STATUS "broflora: using standalone repo at ${BROFLORA_DIR}")
        add_subdirectory("${BROFLORA_DIR}" "${CMAKE_BINARY_DIR}/broflora" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "broflora: using submodule at third_party/broflora")
        add_subdirectory(broflora EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brogameagent (game AI: navmesh, pathfinding, steering, behavior trees) ---
# Prefer the standalone repo at ../brogameagent for local development.
# Falls back to the git submodule at third_party/brogameagent.
# Gated on BRO_WITH_GAMEAI: brogameagent backs the bro.ai.game bindings and is
# embedded by value in the 3D scene graph (agent binding / world ticker), so 3D
# forces GAMEAI on and its transitive dependency is always satisfied.
if(BRO_WITH_GAMEAI)
    bro_default_sibling_dir(brogameagent _brogameagent_default)
    set(BROGAMEAGENT_DIR "${_brogameagent_default}" CACHE PATH "Path to standalone brogameagent repo")
    if(EXISTS "${BROGAMEAGENT_DIR}/CMakeLists.txt")
        message(STATUS "brogameagent: using standalone repo at ${BROGAMEAGENT_DIR}")
        add_subdirectory("${BROGAMEAGENT_DIR}" "${CMAKE_BINARY_DIR}/brogameagent" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brogameagent: using submodule at third_party/brogameagent")
        add_subdirectory(brogameagent EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brolm ---
# Prefer the standalone repo at ../brolm for local development.
# Falls back to the git submodule at third_party/brolm.
# MUST be added before brodiffusion (which depends on brolm). Guards
# bromath/brotensor with if(NOT TARGET ...) so it reuses the already-added targets.
if(BRO_WITH_LM)
    bro_default_sibling_dir(brolm _brolm_default)
    set(BROLM_DIR "${_brolm_default}" CACHE PATH "Path to standalone brolm repo")
    if(EXISTS "${BROLM_DIR}/CMakeLists.txt")
        message(STATUS "brolm: using standalone repo at ${BROLM_DIR}")
        add_subdirectory("${BROLM_DIR}" "${CMAKE_BINARY_DIR}/brolm" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brolm: using submodule at third_party/brolm")
        add_subdirectory(brolm EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brosoundml ---
# Prefer the standalone repo at ../brosoundml for local development.
# Falls back to the git submodule at third_party/brosoundml.
# MUST be added after brolm: a brosoundml tokenizer target links a brolm
# tokenizer target. Guards bromath/brotensor with if(NOT TARGET ...) so it
# reuses the already-added targets.
if(BRO_WITH_SOUNDML)
    bro_default_sibling_dir(brosoundml _brosoundml_default)
    set(BROSOUNDML_DIR "${_brosoundml_default}" CACHE PATH "Path to standalone brosoundml repo")
    if(EXISTS "${BROSOUNDML_DIR}/CMakeLists.txt")
        message(STATUS "brosoundml: using standalone repo at ${BROSOUNDML_DIR}")
        add_subdirectory("${BROSOUNDML_DIR}" "${CMAKE_BINARY_DIR}/brosoundml" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brosoundml: using submodule at third_party/brosoundml")
        add_subdirectory(brosoundml EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brovisionml ---
# Prefer the standalone repo at ../brovisionml for local development.
# Falls back to the git submodule at third_party/brovisionml.
# Depends on bromath + brotensor + broimage; all three are already targets by
# this point. MUST be added before brodiffusion (which depends on brovisionml
# for TripoSplat). Guards with if(NOT TARGET ...) so it reuses those targets.
if(BRO_WITH_VISION AND NOT TARGET brovisionml)
    bro_default_sibling_dir(brovisionml _brovisionml_default)
    set(BROVISIONML_DIR "${_brovisionml_default}" CACHE PATH "Path to standalone brovisionml repo")
    if(EXISTS "${BROVISIONML_DIR}/CMakeLists.txt")
        message(STATUS "brovisionml: using standalone repo at ${BROVISIONML_DIR}")
        add_subdirectory("${BROVISIONML_DIR}" "${CMAKE_BINARY_DIR}/brovisionml" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brovisionml: using submodule at third_party/brovisionml")
        add_subdirectory(brovisionml EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodiffusion ---
# Prefer the standalone repo at ../brodiffusion for local development.
# Falls back to the git submodule at third_party/brodiffusion.
# MUST be added after brolm and brovisionml (which it depends on). Guards bromath/brotensor/brovisionml
# with if(NOT TARGET ...) so it reuses those targets and reads the GPU cache
# vars instead of re-adding or defaulting them.
if(BRO_WITH_DIFFUSION)
    bro_default_sibling_dir(brodiffusion _brodiffusion_default)
    set(BRODIFFUSION_DIR "${_brodiffusion_default}" CACHE PATH "Path to standalone brodiffusion repo")
    if(EXISTS "${BRODIFFUSION_DIR}/CMakeLists.txt")
        message(STATUS "brodiffusion: using standalone repo at ${BRODIFFUSION_DIR}")
        add_subdirectory("${BRODIFFUSION_DIR}" "${CMAKE_BINARY_DIR}/brodiffusion" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brodiffusion: using submodule at third_party/brodiffusion")
        add_subdirectory(brodiffusion EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broconf ---
# Prefer the standalone repo at ../broconf for local development.
# Falls back to the git submodule at third_party/broconf.
if(BRO_WITH_CONF)
    bro_default_sibling_dir(broconf _broconf_default)
    set(BROCONF_DIR "${_broconf_default}" CACHE PATH "Path to standalone broconf repo")
    if(EXISTS "${BROCONF_DIR}/CMakeLists.txt")
        message(STATUS "broconf: using standalone repo at ${BROCONF_DIR}")
        add_subdirectory("${BROCONF_DIR}" "${CMAKE_BINARY_DIR}/broconf" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "broconf: using submodule at third_party/broconf")
        add_subdirectory(broconf EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brothemes ---
if(BRO_WITH_THEMES AND NOT TARGET brothemes)
    set(BROTHEMES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROTHEMES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brothemes _brothemes_default)
    set(BROTHEMES_DIR "${_brothemes_default}" CACHE PATH "Path to standalone brothemes repo")
    if(EXISTS "${BROTHEMES_DIR}/CMakeLists.txt")
        message(STATUS "brothemes: using standalone repo at ${BROTHEMES_DIR}")
        add_subdirectory("${BROTHEMES_DIR}" "${CMAKE_BINARY_DIR}/brothemes" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brothemes/CMakeLists.txt")
        message(STATUS "brothemes: using submodule at third_party/brothemes")
        add_subdirectory(brothemes EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brokeys ---
if(BRO_WITH_KEYS AND NOT TARGET brokeys)
    set(BROKEYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brokeys _brokeys_default)
    set(BROKEYS_DIR "${_brokeys_default}" CACHE PATH "Path to standalone brokeys repo")
    if(EXISTS "${BROKEYS_DIR}/CMakeLists.txt")
        message(STATUS "brokeys: using standalone repo at ${BROKEYS_DIR}")
        add_subdirectory("${BROKEYS_DIR}" "${CMAKE_BINARY_DIR}/brokeys" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brokeys/CMakeLists.txt")
        message(STATUS "brokeys: using submodule at third_party/brokeys")
        add_subdirectory(brokeys EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brosearch ---
if(BRO_WITH_SEARCH AND NOT TARGET brosearch)
    set(BROSEARCH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BROSEARCH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brosearch _brosearch_default)
    set(BROSEARCH_DIR "${_brosearch_default}" CACHE PATH "Path to standalone brosearch repo")
    if(EXISTS "${BROSEARCH_DIR}/CMakeLists.txt")
        message(STATUS "brosearch: using standalone repo at ${BROSEARCH_DIR}")
        add_subdirectory("${BROSEARCH_DIR}" "${CMAKE_BINARY_DIR}/brosearch" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brosearch/CMakeLists.txt")
        message(STATUS "brosearch: using submodule at third_party/brosearch")
        add_subdirectory(brosearch EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broapps ---
if(BRO_WITH_APPS AND NOT TARGET broapps)
    set(BROAPPS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broapps _broapps_default)
    set(BROAPPS_DIR "${_broapps_default}" CACHE PATH "Path to standalone broapps repo")
    if(EXISTS "${BROAPPS_DIR}/CMakeLists.txt")
        message(STATUS "broapps: using standalone repo at ${BROAPPS_DIR}")
        add_subdirectory("${BROAPPS_DIR}" "${CMAKE_BINARY_DIR}/broapps" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broapps/CMakeLists.txt")
        message(STATUS "broapps: using submodule at third_party/broapps")
        add_subdirectory(broapps EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brovfs ---
if(BRO_WITH_VFS AND NOT TARGET brovfs)
    set(BROVFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brovfs _brovfs_default)
    set(BROVFS_DIR "${_brovfs_default}" CACHE PATH "Path to standalone brovfs repo")
    if(EXISTS "${BROVFS_DIR}/CMakeLists.txt")
        message(STATUS "brovfs: using standalone repo at ${BROVFS_DIR}")
        add_subdirectory("${BROVFS_DIR}" "${CMAKE_BINARY_DIR}/brovfs" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brovfs/CMakeLists.txt")
        message(STATUS "brovfs: using submodule at third_party/brovfs")
        add_subdirectory(brovfs EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brothumb ---
if(BRO_WITH_THUMB AND NOT TARGET brothumb)
    set(BROTHUMB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brothumb _brothumb_default)
    set(BROTHUMB_DIR "${_brothumb_default}" CACHE PATH "Path to standalone brothumb repo")
    if(EXISTS "${BROTHUMB_DIR}/CMakeLists.txt")
        message(STATUS "brothumb: using standalone repo at ${BROTHUMB_DIR}")
        add_subdirectory("${BROTHUMB_DIR}" "${CMAKE_BINARY_DIR}/brothumb" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brothumb/CMakeLists.txt")
        message(STATUS "brothumb: using submodule at third_party/brothumb")
        add_subdirectory(brothumb EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broseat ---
if(BRO_WITH_SEAT AND NOT TARGET broseat)
    set(BROSEAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broseat _broseat_default)
    set(BROSEAT_DIR "${_broseat_default}" CACHE PATH "Path to standalone broseat repo")
    if(EXISTS "${BROSEAT_DIR}/CMakeLists.txt")
        message(STATUS "broseat: using standalone repo at ${BROSEAT_DIR}")
        add_subdirectory("${BROSEAT_DIR}" "${CMAKE_BINARY_DIR}/broseat" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broseat/CMakeLists.txt")
        message(STATUS "broseat: using submodule at third_party/broseat")
        add_subdirectory(broseat EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brocred ---
if(BRO_WITH_CRED AND NOT TARGET brocred)
    set(BROCRED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brocred _brocred_default)
    set(BROCRED_DIR "${_brocred_default}" CACHE PATH "Path to standalone brocred repo")
    if(EXISTS "${BROCRED_DIR}/CMakeLists.txt")
        message(STATUS "brocred: using standalone repo at ${BROCRED_DIR}")
        add_subdirectory("${BROCRED_DIR}" "${CMAKE_BINARY_DIR}/brocred" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brocred/CMakeLists.txt")
        message(STATUS "brocred: using submodule at third_party/brocred")
        add_subdirectory(brocred EXCLUDE_FROM_ALL)
    endif()
    if(WIN32 AND TARGET brocred AND EXISTS "${CMAKE_BINARY_DIR}/cppwinrt")
        target_include_directories(brocred PRIVATE "${CMAKE_BINARY_DIR}/cppwinrt")
    endif()
endif()

# --- brosys ---
if(BRO_WITH_SYS AND NOT TARGET brosys)
    set(BROSYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brosys _brosys_default)
    set(BROSYS_DIR "${_brosys_default}" CACHE PATH "Path to standalone brosys repo")
    if(EXISTS "${BROSYS_DIR}/CMakeLists.txt")
        message(STATUS "brosys: using standalone repo at ${BROSYS_DIR}")
        add_subdirectory("${BROSYS_DIR}" "${CMAKE_BINARY_DIR}/brosys" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brosys/CMakeLists.txt")
        message(STATUS "brosys: using submodule at third_party/brosys")
        add_subdirectory(brosys EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodisplays ---
if(BRO_WITH_DISPLAYS AND NOT TARGET brodisplays)
    set(BRODISPLAYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brodisplays _brodisplays_default)
    set(BRODISPLAYS_DIR "${_brodisplays_default}" CACHE PATH "Path to standalone brodisplays repo")
    if(EXISTS "${BRODISPLAYS_DIR}/CMakeLists.txt")
        message(STATUS "brodisplays: using standalone repo at ${BRODISPLAYS_DIR}")
        add_subdirectory("${BRODISPLAYS_DIR}" "${CMAKE_BINARY_DIR}/brodisplays" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brodisplays/CMakeLists.txt")
        message(STATUS "brodisplays: using submodule at third_party/brodisplays")
        add_subdirectory(brodisplays EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broportal ---
if(BRO_WITH_PORTAL AND NOT TARGET broportal)
    set(BROPORTAL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broportal _broportal_default)
    set(BROPORTAL_DIR "${_broportal_default}" CACHE PATH "Path to standalone broportal repo")
    if(EXISTS "${BROPORTAL_DIR}/CMakeLists.txt")
        message(STATUS "broportal: using standalone repo at ${BROPORTAL_DIR}")
        add_subdirectory("${BROPORTAL_DIR}" "${CMAKE_BINARY_DIR}/broportal" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broportal/CMakeLists.txt")
        message(STATUS "broportal: using submodule at third_party/broportal")
        add_subdirectory(broportal EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brocompositor ---
if(BRO_WITH_COMPOSITOR AND NOT TARGET brocompositor)
    set(BROCOMPOSITOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(EXISTS "$ENV{HOME}/opt/bc-wlr/lib/pkgconfig/wlroots-0.18.pc")
        set(ENV{PKG_CONFIG_PATH} "$ENV{HOME}/opt/bc-wlr/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
    bro_default_sibling_dir(brocompositor _brocompositor_default)
    set(BROCOMPOSITOR_DIR "${_brocompositor_default}" CACHE PATH "Path to standalone brocompositor repo")
    if(EXISTS "${BROCOMPOSITOR_DIR}/CMakeLists.txt")
        message(STATUS "brocompositor: using standalone repo at ${BROCOMPOSITOR_DIR}")
        add_subdirectory("${BROCOMPOSITOR_DIR}" "${CMAKE_BINARY_DIR}/brocompositor" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brocompositor/CMakeLists.txt")
        message(STATUS "brocompositor: using submodule at third_party/brocompositor")
        add_subdirectory(brocompositor EXCLUDE_FROM_ALL)
    endif()
    if(WIN32 AND TARGET brocompositor_win AND EXISTS "${CMAKE_BINARY_DIR}/cppwinrt")
        target_include_directories(brocompositor_win PRIVATE "${CMAKE_BINARY_DIR}/cppwinrt")
    endif()
endif()

# --- browl ---
if(BRO_WITH_WL AND NOT TARGET browl)
    set(BROWL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(browl _browl_default)
    set(BROWL_DIR "${_browl_default}" CACHE PATH "Path to standalone browl repo")
    if(EXISTS "${BROWL_DIR}/CMakeLists.txt")
        message(STATUS "browl: using standalone repo at ${BROWL_DIR}")
        add_subdirectory("${BROWL_DIR}" "${CMAKE_BINARY_DIR}/browl" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/browl/CMakeLists.txt")
        message(STATUS "browl: using submodule at third_party/browl")
        add_subdirectory(browl EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broa11y ---
if(BRO_WITH_A11Y AND NOT TARGET broa11y)
    set(BROA11Y_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broa11y _broa11y_default)
    set(BROA11Y_DIR "${_broa11y_default}" CACHE PATH "Path to standalone broa11y repo")
    if(EXISTS "${BROA11Y_DIR}/CMakeLists.txt")
        message(STATUS "broa11y: using standalone repo at ${BROA11Y_DIR}")
        add_subdirectory("${BROA11Y_DIR}" "${CMAKE_BINARY_DIR}/broa11y" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broa11y/CMakeLists.txt")
        message(STATUS "broa11y: using submodule at third_party/broa11y")
        add_subdirectory(broa11y EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodmabuf ---
if(BRO_WITH_DMABUF AND NOT TARGET brodmabuf)
    set(BRODMABUF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brodmabuf _brodmabuf_default)
    set(BRODMABUF_DIR "${_brodmabuf_default}" CACHE PATH "Path to standalone brodmabuf repo")
    if(EXISTS "${BRODMABUF_DIR}/CMakeLists.txt")
        message(STATUS "brodmabuf: using standalone repo at ${BRODMABUF_DIR}")
        add_subdirectory("${BRODMABUF_DIR}" "${CMAKE_BINARY_DIR}/brodmabuf" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brodmabuf/CMakeLists.txt")
        message(STATUS "brodmabuf: using submodule at third_party/brodmabuf")
        add_subdirectory(brodmabuf EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodbus ---
# The shared sd-bus layer of brocred, brosys, broseat, broportal, and brompris: Linux
# only, and only with libsystemd, whose absence those libraries handle
# themselves (brocred and brosys build without D-Bus).
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
if(_bro_wants_brodbus AND NOT TARGET brodbus)
    set(BRODBUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brodbus _brodbus_default)
    set(BRODBUS_DIR "${_brodbus_default}" CACHE PATH "Path to standalone brodbus repo")
    if(EXISTS "${BRODBUS_DIR}/CMakeLists.txt")
        message(STATUS "brodbus: using standalone repo at ${BRODBUS_DIR}")
        add_subdirectory("${BRODBUS_DIR}" "${CMAKE_BINARY_DIR}/brodbus" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brodbus/CMakeLists.txt")
        message(STATUS "brodbus: using submodule at third_party/brodbus")
        add_subdirectory(brodbus EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodecor ---
if(BRO_WITH_DECOR AND NOT TARGET brodecor)
    set(BRODECOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brodecor _brodecor_default)
    set(BRODECOR_DIR "${_brodecor_default}" CACHE PATH "Path to standalone brodecor repo")
    if(EXISTS "${BRODECOR_DIR}/CMakeLists.txt")
        message(STATUS "brodecor: using standalone repo at ${BRODECOR_DIR}")
        add_subdirectory("${BRODECOR_DIR}" "${CMAKE_BINARY_DIR}/brodecor" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brodecor/CMakeLists.txt")
        message(STATUS "brodecor: using submodule at third_party/brodecor")
        add_subdirectory(brodecor EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broclip ---
if(BRO_WITH_CLIP AND NOT TARGET broclip)
    set(BROCLIP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broclip _broclip_default)
    set(BROCLIP_DIR "${_broclip_default}" CACHE PATH "Path to standalone broclip repo")
    if(EXISTS "${BROCLIP_DIR}/CMakeLists.txt")
        message(STATUS "broclip: using standalone repo at ${BROCLIP_DIR}")
        add_subdirectory("${BROCLIP_DIR}" "${CMAKE_BINARY_DIR}/broclip" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broclip/CMakeLists.txt")
        message(STATUS "broclip: using submodule at third_party/broclip")
        add_subdirectory(broclip EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brompris ---
if(BRO_WITH_MPRIS AND NOT TARGET brompris)
    set(BROMPRIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(brompris _brompris_default)
    set(BROMPRIS_DIR "${_brompris_default}" CACHE PATH "Path to standalone brompris repo")
    if(EXISTS "${BROMPRIS_DIR}/CMakeLists.txt")
        message(STATUS "brompris: using standalone repo at ${BROMPRIS_DIR}")
        add_subdirectory("${BROMPRIS_DIR}" "${CMAKE_BINARY_DIR}/brompris" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/brompris/CMakeLists.txt")
        message(STATUS "brompris: using submodule at third_party/brompris")
        add_subdirectory(brompris EXCLUDE_FROM_ALL)
    endif()
endif()

# --- bropulse ---
if(BRO_WITH_PULSE AND NOT TARGET bropulse)
    set(BROPULSE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(bropulse _bropulse_default)
    set(BROPULSE_DIR "${_bropulse_default}" CACHE PATH "Path to standalone bropulse repo")
    if(EXISTS "${BROPULSE_DIR}/CMakeLists.txt")
        message(STATUS "bropulse: using standalone repo at ${BROPULSE_DIR}")
        add_subdirectory("${BROPULSE_DIR}" "${CMAKE_BINARY_DIR}/bropulse" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bropulse/CMakeLists.txt")
        message(STATUS "bropulse: using submodule at third_party/bropulse")
        add_subdirectory(bropulse EXCLUDE_FROM_ALL)
    endif()
endif()

# --- broime ---
if(BRO_WITH_IME AND NOT TARGET broime)
    set(BROIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    bro_default_sibling_dir(broime _broime_default)
    set(BROIME_DIR "${_broime_default}" CACHE PATH "Path to standalone broime repo")
    if(EXISTS "${BROIME_DIR}/CMakeLists.txt")
        message(STATUS "broime: using standalone repo at ${BROIME_DIR}")
        add_subdirectory("${BROIME_DIR}" "${CMAKE_BINARY_DIR}/broime" EXCLUDE_FROM_ALL)
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/broime/CMakeLists.txt")
        message(STATUS "broime: using submodule at third_party/broime")
        add_subdirectory(broime EXCLUDE_FROM_ALL)
    endif()
endif()

# --- Jolt Physics ---
if(BRO_WITH_PHYSICS)
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
add_subdirectory(jolt/Build EXCLUDE_FROM_ALL)
endif()  # BRO_WITH_PHYSICS

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
