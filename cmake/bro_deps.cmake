# bro_deps.cmake: the bro ecosystem's one dependency mechanism.
#
# An identical copy lives in every repo that has dependencies; edit it in bro
# and copy it out with bro's scripts/sync-deps.sh. No git submodules anywhere: a
# dependency resolves, in order,
#
#   1. to a target that already exists, so an outer project's choice wins;
#   2. to the working tree ../<name> beside the top-level project (or beside the
#      project calling this), through FETCHCONTENT_SOURCE_DIR_<NAME>; pass
#      -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> to choose another tree;
#   3. to the GitHub archive tarball of one commit, fetched at configure. The
#      commit is, first that applies:
#        a. the one the top-level project's cmake/bro_lock.cmake names (a
#           release; written by bro's scripts/lock-deps.sh);
#        b. REF, when it is a 40-hex sha (third-party code is pinned this way);
#        c. the head of REF (a branch or tag), or of the default branch when
#           there is no REF, read with `git ls-remote` at every configure. A
#           moved branch is a new commit, hence a new URL, hence a refetch.
#
#   bro_dependency(<name> [GITHUB <owner/repo>] [REF <sha|branch|tag>]
#                  [TARGET <target>] [THIRD_PARTY] [PIN_ONLY | SOURCE_ONLY]
#                  [OPTIONS <K=V>...])
#   bro_dependencies(<name>...)     # declare several, as PIN_ONLY
#   bro_lock(<name> <sha>)          # only in cmake/bro_lock.cmake
#
# GITHUB defaults to wlejon/<name>; THIRD_PARTY code must name it and should pin
# a sha. Declarations are first-wins across the whole build: the first call that
# names <name> fixes its GITHUB, REF and THIRD_PARTY, so the top-level project
# can declare everything up front (PIN_ONLY declares without adding). Declaring
# up front also lets one configure resolve every branch head concurrently: the
# first dependency that needs a head resolves all declared ones that do in a
# single batch of parallel `git ls-remote`s. THIRD_PARTY skips step 2 (a stray
# ../curl must not replace the pinned curl). SOURCE_ONLY fetches without
# add_subdirectory(). OPTIONS are cache entries forced before the dependency is
# added. Afterwards <name>_SOURCE_DIR holds the source tree.
#
# Offline: each resolved head is kept in the build's cache; when `git ls-remote`
# fails (or with -DBRO_DEPS_OFFLINE=ON, or FETCHCONTENT_FULLY_DISCONNECTED), the
# last commit this build directory resolved is reused, with a warning.
#
# Keep a call's name, GITHUB and REF on one line: bro's scripts read them.

# Script mode: one resolver of a concurrent batch (see _bro_deps_resolve).
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED BRO_DEPS_RESOLVE_OUT)
    set(ENV{GIT_TERMINAL_PROMPT} 0)
    set(ENV{GCM_INTERACTIVE} never)
    set(pattern HEAD)
    if(BRO_DEPS_RESOLVE_REF)
        set(pattern "${BRO_DEPS_RESOLVE_REF}")
    endif()
    execute_process(
        COMMAND "${BRO_DEPS_GIT}" ls-remote "https://github.com/${BRO_DEPS_RESOLVE_REPO}.git" "${pattern}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
    set(sha "")
    if(rc EQUAL 0)
        string(REPLACE "\n" ";" lines "${out}")
        # A branch head, else a peeled (annotated) tag, else a lightweight tag.
        foreach(want "refs/heads/${pattern}" "refs/tags/${pattern}^{}" "refs/tags/${pattern}" "${pattern}")
            foreach(line IN LISTS lines)
                if(line MATCHES "^([0-9a-f]+)\t(.*)$" AND CMAKE_MATCH_2 STREQUAL want)
                    set(sha "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
            if(sha)
                break()
            endif()
        endforeach()
    endif()
    file(WRITE "${BRO_DEPS_RESOLVE_OUT}" "${sha}")
    return()
endif()

include_guard(GLOBAL)
include(FetchContent)
set_property(GLOBAL PROPERTY _bro_deps_file "${CMAKE_CURRENT_LIST_FILE}")

# Whether <value> is a full commit sha (CMake's regex has no {n}).
function(_bro_deps_is_sha value out)
    string(LENGTH "${value}" n)
    if(n EQUAL 40 AND value MATCHES "^[0-9a-f]+$")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(bro_lock name sha)
    string(TOLOWER "${name}" lc)
    _bro_deps_is_sha("${sha}" ok)
    if(NOT ok)
        message(FATAL_ERROR "bro_lock(${name} ${sha}): not a 40-hex commit")
    endif()
    set_property(GLOBAL PROPERTY _bro_dep_${lc}_lock "${sha}")
endfunction()

function(bro_dependencies)
    foreach(name IN LISTS ARGN)
        bro_dependency(${name} PIN_ONLY)
    endforeach()
endfunction()

# The working tree a dependency would build from, if any (step 2).
function(_bro_deps_tree name out)
    string(TOUPPER "${name}" uc)
    string(TOLOWER "${name}" lc)
    get_property(third_party GLOBAL PROPERTY _bro_dep_${lc}_third_party)
    set(dir "")
    if(FETCHCONTENT_SOURCE_DIR_${uc})
        set(dir "${FETCHCONTENT_SOURCE_DIR_${uc}}")
    elseif(NOT third_party)
        foreach(root "${CMAKE_SOURCE_DIR}/.." "${PROJECT_SOURCE_DIR}/..")
            get_filename_component(d "${root}/${name}" ABSOLUTE)
            if(EXISTS "${d}/CMakeLists.txt")
                set(dir "${d}")
                break()
            endif()
        endforeach()
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

# Resolve the heads of the named dependencies, concurrently: one `cmake -P`
# child per name, all started at once as the stages of one execute_process()
# pipeline (none reads its stdin), each writing its commit to a file.
function(_bro_deps_resolve names)
    find_package(Git QUIET)
    set(dir "${CMAKE_BINARY_DIR}/CMakeFiles/bro_deps")
    file(MAKE_DIRECTORY "${dir}")
    get_property(self GLOBAL PROPERTY _bro_deps_file)
    set(cmd "")
    foreach(name IN LISTS names)
        get_property(gh GLOBAL PROPERTY _bro_dep_${name}_github)
        get_property(ref GLOBAL PROPERTY _bro_dep_${name}_ref)
        file(REMOVE "${dir}/${name}.txt")
        if(GIT_EXECUTABLE)
            list(APPEND cmd COMMAND "${CMAKE_COMMAND}" "-DBRO_DEPS_GIT=${GIT_EXECUTABLE}"
                 "-DBRO_DEPS_RESOLVE_REPO=${gh}" "-DBRO_DEPS_RESOLVE_REF=${ref}"
                 "-DBRO_DEPS_RESOLVE_OUT=${dir}/${name}.txt" -P "${self}")
        else()
            # No git: one GitHub API call each (unauthenticated: 60 an hour).
            if(NOT ref)
                set(ref HEAD)
            endif()
            file(DOWNLOAD "https://api.github.com/repos/${gh}/commits/${ref}" "${dir}/${name}.txt"
                 HTTPHEADER "Accept: application/vnd.github.sha" TIMEOUT 60 STATUS st)
        endif()
    endforeach()
    if(cmd)
        list(LENGTH names n)
        string(REPLACE ";" ", " shown "${names}")
        message(STATUS "bro_deps: resolving ${n} branch head(s): ${shown}")
        execute_process(${cmd} TIMEOUT 120 OUTPUT_QUIET ERROR_QUIET)
    endif()
    foreach(name IN LISTS names)
        set(sha "")
        if(EXISTS "${dir}/${name}.txt")
            file(READ "${dir}/${name}.txt" sha)
            string(STRIP "${sha}" sha)
        endif()
        _bro_deps_is_sha("${sha}" ok)
        if(ok)
            set_property(GLOBAL PROPERTY _bro_dep_${name}_resolved "${sha}")
        else()
            set_property(GLOBAL PROPERTY _bro_dep_${name}_resolved FAILED)
        endif()
    endforeach()
endfunction()

# The commit to fetch for <lc>, and how it was chosen.
function(_bro_deps_commit lc out_sha out_how)
    get_property(gh GLOBAL PROPERTY _bro_dep_${lc}_github)
    get_property(ref GLOBAL PROPERTY _bro_dep_${lc}_ref)
    get_property(lock GLOBAL PROPERTY _bro_dep_${lc}_lock)
    if(lock)
        set(${out_sha} "${lock}" PARENT_SCOPE)
        set(${out_how} "locked in cmake/bro_lock.cmake" PARENT_SCOPE)
        return()
    endif()
    _bro_deps_is_sha("${ref}" pinned)
    if(pinned)
        set(${out_sha} "${ref}" PARENT_SCOPE)
        set(${out_how} "pinned" PARENT_SCOPE)
        return()
    endif()
    set(what "${ref}")
    if(NOT what)
        set(what "default branch")
    endif()
    set(last_key "${gh}@${ref}")
    set(offline OFF)
    if(BRO_DEPS_OFFLINE OR FETCHCONTENT_FULLY_DISCONNECTED)
        set(offline ON)
    endif()
    get_property(resolved GLOBAL PROPERTY _bro_dep_${lc}_resolved)
    if(NOT resolved AND NOT offline)
        # Resolve every declared dependency that will need a head, in one batch.
        get_property(all GLOBAL PROPERTY _bro_deps_names)
        set(batch ${lc})
        foreach(n IN LISTS all)
            if(n STREQUAL lc)
                continue()
            endif()
            get_property(r GLOBAL PROPERTY _bro_dep_${n}_resolved)
            get_property(l GLOBAL PROPERTY _bro_dep_${n}_lock)
            get_property(nref GLOBAL PROPERTY _bro_dep_${n}_ref)
            get_property(src GLOBAL PROPERTY _bro_dep_${n}_src)
            get_property(nm GLOBAL PROPERTY _bro_dep_${n}_name)
            _bro_deps_tree("${nm}" tree)
            _bro_deps_is_sha("${nref}" npinned)
            if(NOT r AND NOT l AND NOT src AND NOT tree AND NOT npinned)
                list(APPEND batch ${n})
            endif()
        endforeach()
        _bro_deps_resolve("${batch}")
        get_property(resolved GLOBAL PROPERTY _bro_dep_${lc}_resolved)
    endif()
    if(resolved AND NOT resolved STREQUAL "FAILED")
        set(_BRO_DEPS_LAST_${lc} "${last_key} ${resolved}" CACHE INTERNAL "")
        set(${out_sha} "${resolved}" PARENT_SCOPE)
        set(${out_how} "${what}" PARENT_SCOPE)
        return()
    endif()
    # Offline, or the lookup failed: the last commit this build resolved.
    if(_BRO_DEPS_LAST_${lc} MATCHES "^([^ ]+) ([0-9a-f]+)$" AND CMAKE_MATCH_1 STREQUAL last_key)
        set(sha "${CMAKE_MATCH_2}")
        get_property(warned GLOBAL PROPERTY _bro_deps_offline_warned)
        if(NOT warned)
            set_property(GLOBAL PROPERTY _bro_deps_offline_warned TRUE)
            if(offline)
                set(why "offline (BRO_DEPS_OFFLINE or FETCHCONTENT_FULLY_DISCONNECTED)")
            else()
                set(why "could not read branch heads from GitHub (offline?)")
            endif()
            message(WARNING "bro_deps: ${why}: dependencies marked OFFLINE below build "
                "the last commit this build directory resolved, which may be behind their branch.")
        endif()
        set(${out_sha} "${sha}" PARENT_SCOPE)
        set(${out_how} "${what}, OFFLINE: last resolved" PARENT_SCOPE)
        return()
    endif()
    message(FATAL_ERROR "bro_dependency(${lc}): could not read the ${what} head of github.com/${gh}, "
        "and this build directory has never resolved one to fall back on. Check the network, "
        "or put a working tree at ../<name> (or -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path>).")
endfunction()

function(bro_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 A "THIRD_PARTY;PIN_ONLY;SOURCE_ONLY" "GITHUB;REF;TARGET" "OPTIONS")
    string(TOLOWER "${name}" lc)
    string(TOUPPER "${name}" uc)
    set(P _bro_dep_${lc})

    # The top-level project's lock, read once.
    get_property(lock_read GLOBAL PROPERTY _bro_deps_lock_read)
    if(NOT lock_read)
        set_property(GLOBAL PROPERTY _bro_deps_lock_read TRUE)
        if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/bro_lock.cmake")
            include("${CMAKE_SOURCE_DIR}/cmake/bro_lock.cmake")
            message(STATUS "bro_deps: ${CMAKE_SOURCE_DIR}/cmake/bro_lock.cmake locks the ecosystem dependencies")
        endif()
    endif()

    # First declaration wins.
    get_property(declared GLOBAL PROPERTY ${P}_github SET)
    if(NOT declared)
        set(gh "${A_GITHUB}")
        if(NOT gh)
            if(A_THIRD_PARTY)
                message(FATAL_ERROR "bro_dependency(${name}): THIRD_PARTY needs GITHUB <owner/repo>")
            endif()
            set(gh "wlejon/${name}")
        endif()
        set_property(GLOBAL PROPERTY ${P}_github "${gh}")
        set_property(GLOBAL PROPERTY ${P}_ref "${A_REF}")
        set_property(GLOBAL PROPERTY ${P}_third_party ${A_THIRD_PARTY})
        set_property(GLOBAL PROPERTY ${P}_name "${name}")
        set_property(GLOBAL APPEND PROPERTY _bro_deps_names ${lc})
    endif()
    if(A_PIN_ONLY)
        return()
    endif()
    if(NOT A_TARGET)
        set(A_TARGET ${name})
    endif()
    get_property(src GLOBAL PROPERTY ${P}_src)
    get_property(added GLOBAL PROPERTY ${P}_added)
    if(added OR (TARGET ${A_TARGET} AND NOT A_SOURCE_ONLY))
        set(${name}_SOURCE_DIR "${src}" PARENT_SCOPE)
        return()
    endif()
    if(NOT src)
        get_property(gh GLOBAL PROPERTY ${P}_github)
        _bro_deps_tree("${name}" tree)
        if(tree)
            set(FETCHCONTENT_SOURCE_DIR_${uc} "${tree}")
            # Never downloaded: FetchContent takes the tree instead.
            set(url "https://github.com/${gh}/archive/HEAD.tar.gz")
        else()
            _bro_deps_commit(${lc} sha how)
            set(url "https://github.com/${gh}/archive/${sha}.tar.gz")
        endif()
        # SOURCE_SUBDIR names nothing, so FetchContent only populates and the
        # add_subdirectory() below decides EXCLUDE_FROM_ALL and the binary dir.
        # Extraction-time timestamps, so a changed commit rebuilds what it changed.
        FetchContent_Declare(${lc} URL "${url}" DOWNLOAD_EXTRACT_TIMESTAMP FALSE
                             SOURCE_SUBDIR _bro_deps_populate_only)
        FetchContent_MakeAvailable(${lc})
        FetchContent_GetProperties(${lc} SOURCE_DIR src)
        set_property(GLOBAL PROPERTY ${P}_src "${src}")
        if(tree)
            get_property(lock GLOBAL PROPERTY ${P}_lock)
            if(lock)
                message(STATUS "${name}: working tree ${src} (the lock's ${lock} is not used)")
            else()
                message(STATUS "${name}: working tree ${src}")
            endif()
        else()
            message(STATUS "${name}: github.com/${gh} ${sha} (${how})")
        endif()
    endif()
    set(${name}_SOURCE_DIR "${src}" PARENT_SCOPE)
    if(A_SOURCE_ONLY OR TARGET ${A_TARGET})
        return()
    endif()
    foreach(opt IN LISTS A_OPTIONS)
        string(REGEX MATCH "^([^=]+)=(.*)$" _ "${opt}")
        set(key "${CMAKE_MATCH_1}")
        set(value "${CMAKE_MATCH_2}")
        set(type STRING)
        if(value MATCHES "^(ON|OFF|TRUE|FALSE|YES|NO|0|1)$")
            set(type BOOL)
        endif()
        set(${key} "${value}" CACHE ${type} "" FORCE)
    endforeach()
    set_property(GLOBAL PROPERTY ${P}_added TRUE)
    # Offline, a dependency's own FetchContent (osqp's git qdldl) must not try
    # to update either; this scope is the one its subdirectory inherits.
    get_property(offline GLOBAL PROPERTY _bro_deps_offline_warned)
    if(offline OR BRO_DEPS_OFFLINE)
        set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
    endif()
    add_subdirectory("${src}" "${FETCHCONTENT_BASE_DIR}/${lc}-build" EXCLUDE_FROM_ALL)
endfunction()
