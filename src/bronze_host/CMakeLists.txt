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
    dom_globals.cpp
)
target_include_directories(bro_bronze_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bro_bronze_host
    PUBLIC bro_engine bro_webgl glad bronze::embed bronze::runtime
)

# --- The host executable ---
# A lib-plus-main whose missing piece is the compiled app: bronze emits an
# object file exporting `bronze_main` (the src/rt/rt.cpp symbol convention;
# embed_run.cpp holds the one reference), and whoever builds an app points
# BRO_BRONZE_APP_OBJ at it. Without it the link would fail on that symbol, so
# the executable simply is not defined until an app object is supplied.
set(BRO_BRONZE_APP_OBJ "" CACHE FILEPATH
    "Object file of a bronze-compiled app (exports bronze_main) to link into bro-bronze-host")
if(BRO_BRONZE_APP_OBJ)
    if(NOT EXISTS "${BRO_BRONZE_APP_OBJ}")
        message(FATAL_ERROR "BRO_BRONZE_APP_OBJ does not exist: ${BRO_BRONZE_APP_OBJ}")
    endif()
    add_executable(bro-bronze-host host_main.cpp "${BRO_BRONZE_APP_OBJ}")
    target_link_libraries(bro-bronze-host PRIVATE bro_bronze_host)
    if(MSVC)
        # Same stack headroom as bro.exe: the engine still hosts QuickJS
        # realms (system panels, iframes) beside the compiled app.
        target_link_options(bro-bronze-host PRIVATE /STACK:8388608)
    endif()
else()
    message(STATUS "bro-bronze-host: no BRO_BRONZE_APP_OBJ supplied; building the "
                   "bro_bronze_host library only")
endif()
