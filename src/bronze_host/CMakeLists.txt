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
    host_events.cpp
    host_timers.cpp
    host_image.cpp
    host_xhr.cpp
)
target_include_directories(bro_bronze_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# broimage is named explicitly rather than inherited: host_image.cpp includes
# broimage/decode.h directly, and a dependency that only works because someone
# else happens to link it is one refactor away from a broken build.
target_link_libraries(bro_bronze_host
    PUBLIC bro_engine bro_webgl glad broimage::broimage bronze::embed bronze::runtime
)

# --- The host executable ---
# A lib-plus-main whose missing piece is the compiled app: bronze emits an
# object file exporting `bronze_main` (the src/rt/rt.cpp symbol convention;
# embed_run.cpp holds the one reference), and whoever builds an app points
# BRO_BRONZE_APP_OBJ at it. Without it the link would fail on that symbol, so
# the executable simply is not defined until an app object is supplied.
set(BRO_BRONZE_APP_OBJ "" CACHE FILEPATH
    "Object file of a bronze-compiled app (exports bronze_main) to link into bro-bronze-host")

# --- Optional: build the app object here instead of by hand ---
# OFF by default, and it stays that way: a build that shells out to ANOTHER
# COMPILER is not something anyone should acquire by configuring bro. Turning
# it on makes the bronze CLI a build dependency of bro-bronze-host and runs it
# on every configure-clean build, which is a ~1 s compile for the scene-graph
# app and minutes for a three.js graph.
#
# The contract is exactly the one README.md documents by hand, so the two
# cannot drift: `bronze build <app> -o <obj> --emit-obj --host-globals
# threejs_host.globals`.
option(BRO_BRONZE_BUILD_APP
       "Compile BRO_BRONZE_APP_SOURCE with the bronze CLI at build time" OFF)
set(BRO_BRONZE_APP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/app/main_scenegraph.js"
    CACHE FILEPATH "App entry .js compiled when BRO_BRONZE_BUILD_APP is ON")

if(BRO_BRONZE_BUILD_APP)
    if(BRO_BRONZE_APP_OBJ)
        message(FATAL_ERROR
            "BRO_BRONZE_BUILD_APP=ON and BRO_BRONZE_APP_OBJ are mutually exclusive: "
            "one compiles the app here, the other adopts an object someone else "
            "compiled. Pick whichever names where the object comes from.")
    endif()
    if(NOT EXISTS "${BRO_BRONZE_APP_SOURCE}")
        message(FATAL_ERROR "BRO_BRONZE_APP_SOURCE does not exist: ${BRO_BRONZE_APP_SOURCE}")
    endif()
    set(_bronze_app_obj "${CMAKE_CURRENT_BINARY_DIR}/bronze_app.obj")
    # The bronze CLI is EXCLUDE_FROM_ALL (see the add_subdirectory above), so it
    # is named as a DEPENDS here to pull it into the build — and only into this
    # build, when this option is on.
    add_custom_command(
        OUTPUT "${_bronze_app_obj}"
        COMMAND bronze build "${BRO_BRONZE_APP_SOURCE}"
                -o "${_bronze_app_obj}"
                --emit-obj
                --host-globals "${CMAKE_CURRENT_SOURCE_DIR}/threejs_host.globals"
        DEPENDS bronze "${BRO_BRONZE_APP_SOURCE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/threejs_host.globals"
        COMMENT "bronze: compiling ${BRO_BRONZE_APP_SOURCE} to an object"
        VERBATIM)
    # DEPENDS on the custom command's OUTPUT is what makes it run: an object
    # listed in add_executable's sources is an input, not a rule.
    add_custom_target(bro-bronze-app-obj DEPENDS "${_bronze_app_obj}")
    set(BRO_BRONZE_APP_OBJ "${_bronze_app_obj}")
endif()

if(BRO_BRONZE_APP_OBJ)
    # An adopted object must already exist — a path typo is otherwise a link
    # error naming a symbol instead of a configure error naming the path. One
    # this build is about to PRODUCE cannot exist yet, so that case is skipped.
    if(NOT BRO_BRONZE_BUILD_APP AND NOT EXISTS "${BRO_BRONZE_APP_OBJ}")
        message(FATAL_ERROR "BRO_BRONZE_APP_OBJ does not exist: ${BRO_BRONZE_APP_OBJ}")
    endif()
    add_executable(bro-bronze-host host_main.cpp "${BRO_BRONZE_APP_OBJ}")
    if(BRO_BRONZE_BUILD_APP)
        # An object produced by a custom command in the same directory is
        # already an ordering dependency, but naming the target is what makes a
        # multi-config generator sequence it before the link.
        add_dependencies(bro-bronze-host bro-bronze-app-obj)
        set_source_files_properties("${BRO_BRONZE_APP_OBJ}" PROPERTIES
                                    GENERATED TRUE EXTERNAL_OBJECT TRUE)
    endif()
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
