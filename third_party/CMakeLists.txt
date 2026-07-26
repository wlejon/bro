# --- QuickJS ---
# Same standalone-repo-wins pattern as the siblings below, with one wrinkle:
# the sibling directory is `../quickjs-ng`, not `../quickjs`. Upstream is
# quickjs-ng, and `../quickjs` is commonly a checkout of Bellard's original
# quickjs, which is a different project and must not be picked up here.
set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
set(QUICKJS_DIR "${CMAKE_SOURCE_DIR}/../quickjs-ng" CACHE PATH "quickjs-ng standalone repo")
if(EXISTS "${QUICKJS_DIR}/quickjs.c")
    message(STATUS "QuickJS: building from standalone repo ${QUICKJS_DIR}")
    set(BRO_QUICKJS_SRC "${QUICKJS_DIR}")
else()
    set(BRO_QUICKJS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/quickjs")
endif()

# MSVC compiles the QuickJS interpreter badly. Same source, same flags
# (/MD /O2 /Ob2 /DNDEBUG), MSVC vs clang-cl: three.js updateMatrixWorld 1.36x,
# 20k Matrix4 multiply 1.51x, and up to 2.79x on branch-heavy microbenchmarks.
# The bytecode loop is one enormous function and MSVC does not do well by it.
# (It is NOT about computed goto — clang-cl defines _MSC_VER, so it takes the
# same `switch` dispatch path. It simply generates better code for it.)
#
# CMake's VS generator sets the toolset per *solution*, so building one target
# with a different compiler requires a nested build. That is contained here:
# qjs is a plain C static library with a stable C ABI, no dependencies, and the
# same /MD runtime, so linking a clang-cl-built qjs.lib into the MSVC build
# changes nothing else in the tree.
set(BRO_CLANG_CL_EXE "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/Llvm/x64/bin/clang-cl.exe")
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio" AND EXISTS "${BRO_CLANG_CL_EXE}")
    set(_bro_qjs_clang_default ON)
else()
    set(_bro_qjs_clang_default OFF)
endif()
option(BRO_QUICKJS_CLANG_CL
       "Build the QuickJS static library with clang-cl (much faster interpreter)"
       ${_bro_qjs_clang_default})

if(BRO_QUICKJS_CLANG_CL)
    message(STATUS "QuickJS: building qjs.lib with clang-cl (nested build)")
    include(ExternalProject)
    # Debug keeps ENABLE_DUMPS so JS_SetDumpFlags(JS_DUMP_LEAKS) still prints
    # leaked GC objects before JS_FreeRuntime asserts.
    ExternalProject_Add(quickjs_clangcl
        SOURCE_DIR "${BRO_QUICKJS_SRC}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/quickjs-clangcl"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
        CMAKE_GENERATOR_TOOLSET "ClangCL"
        CMAKE_ARGS
            -DQJS_BUILD_CLI=OFF
            -DQJS_BUILD_EXAMPLES=OFF
            -DQJS_ENABLE_INSTALL=OFF
            -DBUILD_QJS_LIBC=ON
            "-DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG} /DENABLE_DUMPS"
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target qjs
        BUILD_ALWAYS 1            # let the nested build do its own up-to-date check
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/quickjs-clangcl/$<CONFIG>/qjs.lib"
        USES_TERMINAL_BUILD 1
    )
    # IMPORTED_LOCATION takes no generator expression, so the per-config
    # properties have to be spelled out; the nested build writes each config to
    # its own subdirectory the same way the outer one does.
    add_library(bro_qjs_clangcl STATIC IMPORTED GLOBAL)
    set(_qjs_cfgs ${CMAKE_CONFIGURATION_TYPES})
    if(NOT _qjs_cfgs)
        set(_qjs_cfgs ${CMAKE_BUILD_TYPE})
    endif()
    set_property(TARGET bro_qjs_clangcl PROPERTY IMPORTED_CONFIGURATIONS "")
    foreach(_cfg IN LISTS _qjs_cfgs)
        string(TOUPPER "${_cfg}" _CFG)
        set_property(TARGET bro_qjs_clangcl APPEND PROPERTY IMPORTED_CONFIGURATIONS ${_CFG})
        set_target_properties(bro_qjs_clangcl PROPERTIES
            IMPORTED_LOCATION_${_CFG} "${CMAKE_BINARY_DIR}/quickjs-clangcl/${_cfg}/qjs.lib")
    endforeach()
    # `qjs` stays the name everything links (bro, brokit and qjsbind all just
    # look for a target called qjs); INTERFACE rather than IMPORTED so the
    # nested build can be an ordering dependency that propagates to consumers.
    add_library(qjs INTERFACE)
    target_link_libraries(qjs INTERFACE bro_qjs_clangcl)
    target_include_directories(qjs INTERFACE "${BRO_QUICKJS_SRC}")
    add_dependencies(qjs quickjs_clangcl)
else()
    add_subdirectory("${BRO_QUICKJS_SRC}" "${CMAKE_BINARY_DIR}/quickjs" EXCLUDE_FROM_ALL)
    target_compile_definitions(qjs PRIVATE $<$<CONFIG:Debug>:ENABLE_DUMPS>)
endif()

# --- FastNoise2 (SIMD noise generation) ---
# Used directly by bro's terrain system. The source lives inside brokit's
# third_party; brokit guards its own add_subdirectory with if(NOT TARGET FastNoise)
# so this won't conflict.
if(NOT TARGET FastNoise)
    set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "" FORCE)
    set(FASTNOISE2_TESTS OFF CACHE BOOL "" FORCE)
    # Resolve via brokit's tree (standalone or submodule).
    set(_BROKIT_ROOT "${CMAKE_SOURCE_DIR}/../brokit")
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
    set(BROMATH_DIR "${CMAKE_SOURCE_DIR}/../bromath" CACHE PATH "Path to standalone bromath repo")
    if(EXISTS "${BROMATH_DIR}/CMakeLists.txt")
        message(STATUS "bromath: using standalone repo at ${BROMATH_DIR}")
        add_subdirectory("${BROMATH_DIR}" "${CMAKE_BINARY_DIR}/bromath" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "bromath: using submodule at third_party/bromath")
        add_subdirectory(bromath EXCLUDE_FROM_ALL)
    endif()
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
    set(BROTENSOR_DIR "${CMAKE_SOURCE_DIR}/../brotensor" CACHE PATH "Path to standalone brotensor repo")
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
set(BROIMAGE_DIR "${CMAKE_SOURCE_DIR}/../broimage" CACHE PATH "Path to standalone broimage repo")
if(EXISTS "${BROIMAGE_DIR}/CMakeLists.txt")
    message(STATUS "broimage: using standalone repo at ${BROIMAGE_DIR}")
    add_subdirectory("${BROIMAGE_DIR}" "${CMAKE_BINARY_DIR}/broimage" EXCLUDE_FROM_ALL)
else()
    message(STATUS "broimage: using submodule at third_party/broimage")
    add_subdirectory(broimage EXCLUDE_FROM_ALL)
endif()

# --- brokit (JS runtime APIs) ---
# Prefer the standalone repo at ../brokit for local development.
# Falls back to the git submodule at third_party/brokit.
set(BROKIT_DIR "${CMAKE_SOURCE_DIR}/../brokit" CACHE PATH "Path to standalone brokit repo")
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
set(HTMLAYOUT_DIR "${CMAKE_SOURCE_DIR}/../htmlayout" CACHE PATH "Path to standalone htmlayout repo")
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

# --- glad2 (OpenGL 3.3 Core loader) ---
add_subdirectory(glad EXCLUDE_FROM_ALL)

# --- broaudio (audio engine library) ---
# Prefer the standalone repo at ../broaudio for local development.
# Falls back to the git submodule at third_party/broaudio.
set(BROAUDIO_DIR "${CMAKE_SOURCE_DIR}/../broaudio" CACHE PATH "Path to standalone broaudio repo")
if(EXISTS "${BROAUDIO_DIR}/CMakeLists.txt")
    message(STATUS "broaudio: using standalone repo at ${BROAUDIO_DIR}")
    add_subdirectory("${BROAUDIO_DIR}" "${CMAKE_BINARY_DIR}/broaudio" EXCLUDE_FROM_ALL)
else()
    message(STATUS "broaudio: using submodule at third_party/broaudio")
    add_subdirectory(broaudio EXCLUDE_FROM_ALL)
endif()

# --- qjsbind (C++20 QuickJS binding library, header-only) ---
# Prefer the standalone repo at ../qjsbind for local development.
set(QJSBIND_DIR "${CMAKE_SOURCE_DIR}/../qjsbind" CACHE PATH "Path to standalone qjsbind repo")
if(EXISTS "${QJSBIND_DIR}/CMakeLists.txt")
    message(STATUS "qjsbind: using standalone repo at ${QJSBIND_DIR}")
    add_subdirectory("${QJSBIND_DIR}" "${CMAKE_BINARY_DIR}/qjsbind" EXCLUDE_FROM_ALL)
else()
    message(STATUS "qjsbind: using submodule at third_party/qjsbind")
    add_subdirectory(qjsbind EXCLUDE_FROM_ALL)
endif()

# --- bromesh (mesh generation + manipulation library) ---
# Prefer the standalone repo at ../bromesh for local development.
# Falls back to the git submodule at third_party/bromesh.
# Gated on BRO_WITH_3D: bromesh backs the 3D scene graph (mesh/terrain/tile
# nodes) and the Mesh JS bindings, all of which are 3D-only. broflora forces
# 3D on, so its transitive bromesh dependency is always satisfied.
if(BRO_WITH_3D)
    set(BROMESH_DIR "${CMAKE_SOURCE_DIR}/../bromesh" CACHE PATH "Path to standalone bromesh repo")
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
    set(BROFLORA_DIR "${CMAKE_SOURCE_DIR}/../broflora" CACHE PATH "Path to standalone broflora repo")
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
    set(BROGAMEAGENT_DIR "${CMAKE_SOURCE_DIR}/../brogameagent" CACHE PATH "Path to standalone brogameagent repo")
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
    set(BROLM_DIR "${CMAKE_SOURCE_DIR}/../brolm" CACHE PATH "Path to standalone brolm repo")
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
    set(BROSOUNDML_DIR "${CMAKE_SOURCE_DIR}/../brosoundml" CACHE PATH "Path to standalone brosoundml repo")
    if(EXISTS "${BROSOUNDML_DIR}/CMakeLists.txt")
        message(STATUS "brosoundml: using standalone repo at ${BROSOUNDML_DIR}")
        add_subdirectory("${BROSOUNDML_DIR}" "${CMAKE_BINARY_DIR}/brosoundml" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brosoundml: using submodule at third_party/brosoundml")
        add_subdirectory(brosoundml EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brodiffusion ---
# Prefer the standalone repo at ../brodiffusion for local development.
# Falls back to the git submodule at third_party/brodiffusion.
# MUST be added after brolm (which it depends on). Guards bromath/brotensor
# with if(NOT TARGET ...) so it reuses those targets and reads the GPU cache
# vars instead of re-adding or defaulting them.
if(BRO_WITH_DIFFUSION)
    set(BRODIFFUSION_DIR "${CMAKE_SOURCE_DIR}/../brodiffusion" CACHE PATH "Path to standalone brodiffusion repo")
    if(EXISTS "${BRODIFFUSION_DIR}/CMakeLists.txt")
        message(STATUS "brodiffusion: using standalone repo at ${BRODIFFUSION_DIR}")
        add_subdirectory("${BRODIFFUSION_DIR}" "${CMAKE_BINARY_DIR}/brodiffusion" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brodiffusion: using submodule at third_party/brodiffusion")
        add_subdirectory(brodiffusion EXCLUDE_FROM_ALL)
    endif()
endif()

# --- brovisionml ---
# Prefer the standalone repo at ../brovisionml for local development.
# Falls back to the git submodule at third_party/brovisionml.
# Depends on bromath + brotensor + broimage; all three are already targets by
# this point. Guards all three with if(NOT TARGET ...) so it reuses those
# targets and reads the GPU cache vars.
if(BRO_WITH_VISION)
    set(BROVISIONML_DIR "${CMAKE_SOURCE_DIR}/../brovisionml" CACHE PATH "Path to standalone brovisionml repo")
    if(EXISTS "${BROVISIONML_DIR}/CMakeLists.txt")
        message(STATUS "brovisionml: using standalone repo at ${BROVISIONML_DIR}")
        add_subdirectory("${BROVISIONML_DIR}" "${CMAKE_BINARY_DIR}/brovisionml" EXCLUDE_FROM_ALL)
    else()
        message(STATUS "brovisionml: using submodule at third_party/brovisionml")
        add_subdirectory(brovisionml EXCLUDE_FROM_ALL)
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
    find_package(GameNetworkingSockets CONFIG REQUIRED)
endif()

# --- Skia (pre-built, optional) ---
include(skia/skia.cmake)

# --- Skia modules: svg, skshaper (+ HarfBuzz/skunicode), skresources ---
#     (compiled from source against pre-built Skia)
include(skia/skia_modules.cmake)
