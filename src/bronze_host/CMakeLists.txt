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
    host_dom_events.cpp
    host_timers.cpp
    host_image.cpp
    host_xhr.cpp
    host_fetch.cpp
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
# web_host.globals`.
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
                --host-globals "${CMAKE_CURRENT_SOURCE_DIR}/web_host.globals"
        DEPENDS bronze "${BRO_BRONZE_APP_SOURCE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/web_host.globals"
        COMMENT "bronze: compiling ${BRO_BRONZE_APP_SOURCE} to an object"
        VERBATIM)
    # DEPENDS on the custom command's OUTPUT is what makes it run: an object
    # listed in add_executable's sources is an input, not a rule.
    add_custom_target(bro-bronze-app-obj DEPENDS "${_bronze_app_obj}")
    set(BRO_BRONZE_APP_OBJ "${_bronze_app_obj}")
endif()

# --- One host executable, given one app object ---
# host_main.cpp is the whole of every one of them: the app is the linked
# object, so "another app" is another link of the same source with a different
# object beside it. Factored into a function rather than repeated because the
# stack option and the library are properties of the HOST, not of any app, and
# a second copy of them is a second thing to forget.
function(bro_bronze_add_host target objPath)
    add_executable(${target} host_main.cpp "${objPath}")
    target_link_libraries(${target} PRIVATE bro_bronze_host)
    if(MSVC)
        # Same stack headroom as bro.exe: the engine still hosts QuickJS
        # realms (system panels, iframes) beside the compiled app.
        target_link_options(${target} PRIVATE /STACK:8388608)
    endif()
endfunction()

if(BRO_BRONZE_APP_OBJ)
    # An adopted object must already exist — a path typo is otherwise a link
    # error naming a symbol instead of a configure error naming the path. One
    # this build is about to PRODUCE cannot exist yet, so that case is skipped.
    if(NOT BRO_BRONZE_BUILD_APP AND NOT EXISTS "${BRO_BRONZE_APP_OBJ}")
        message(FATAL_ERROR "BRO_BRONZE_APP_OBJ does not exist: ${BRO_BRONZE_APP_OBJ}")
    endif()
    bro_bronze_add_host(bro-bronze-host "${BRO_BRONZE_APP_OBJ}")
    if(BRO_BRONZE_BUILD_APP)
        # An object produced by a custom command in the same directory is
        # already an ordering dependency, but naming the target is what makes a
        # multi-config generator sequence it before the link.
        add_dependencies(bro-bronze-host bro-bronze-app-obj)
        set_source_files_properties("${BRO_BRONZE_APP_OBJ}" PROPERTIES
                                    GENERATED TRUE EXTERNAL_OBJECT TRUE)
    endif()
else()
    message(STATUS "bro-bronze-host: no BRO_BRONZE_APP_OBJ supplied; building the "
                   "bro_bronze_host library only")
endif()

# --- Additional host executables, one per app ---
# BRO_BRONZE_APPS is a semicolon list of `name=objpath` pairs, each producing
# `bro-bronze-host-<name>`. It exists BESIDE BRO_BRONZE_APP_OBJ rather than
# replacing it: the pinned check names bro-bronze-host and must keep finding
# exactly that binary, and one app is still the common case. The two are not
# exclusive — a tree can have the scene-graph app as bro-bronze-host and an
# events probe as bro-bronze-host-events at the same time, which is what makes
# a second pinned check possible without reconfiguring between runs.
#
#     -DBRO_BRONZE_APPS="events=D:/build/events.obj;lit=D:/build/lit.obj"
#
# A pair with no `=`, an empty half, or a path that does not exist is a
# configure error naming the pair: a mistyped one would otherwise become a
# missing target, and "the app is broken" and "the app was never built" would
# read the same at the test.
set(BRO_BRONZE_APPS "" CACHE STRING
    "Semicolon list of name=objpath pairs; each builds bro-bronze-host-<name>")

foreach(_app IN LISTS BRO_BRONZE_APPS)
    if(NOT _app MATCHES "^([^=]+)=(.+)$")
        message(FATAL_ERROR
            "BRO_BRONZE_APPS entry is not `name=objpath`: ${_app}")
    endif()
    set(_app_name "${CMAKE_MATCH_1}")
    set(_app_obj "${CMAKE_MATCH_2}")
    if(NOT EXISTS "${_app_obj}")
        message(FATAL_ERROR
            "BRO_BRONZE_APPS entry '${_app_name}': object does not exist: ${_app_obj}")
    endif()
    if(TARGET bro-bronze-host-${_app_name})
        message(FATAL_ERROR
            "BRO_BRONZE_APPS names '${_app_name}' twice")
    endif()
    bro_bronze_add_host(bro-bronze-host-${_app_name} "${_app_obj}")
    message(STATUS "bro-bronze-host-${_app_name}: ${_app_obj}")
endforeach()
