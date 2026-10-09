# bro_deps.cmake: the bro ecosystem's one dependency mechanism.
#
# An identical copy lives in every repo that has dependencies; edit it in bro
# and copy it out. No git submodules anywhere: a dependency resolves, in order,
#
#   1. to a target that already exists, so an outer project's choice wins;
#   2. to the working tree ../<name> beside the top-level project (or beside the
#      project calling this), through FETCHCONTENT_SOURCE_DIR_<NAME>; pass
#      -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> to choose another tree;
#   3. to the GitHub archive tarball of the pinned commit, fetched at configure.
#
#   bro_dependency(<name> [GITHUB <owner/repo> REF <sha>] [TARGET <target>]
#                  [THIRD_PARTY] [PIN_ONLY | SOURCE_ONLY] [OPTIONS <K=V>...])
#
# Pins are first-declaration-wins across the whole build: the top-level project
# declares its pins (PIN_ONLY declares without adding) before it adds anything,
# so a nested project's pin for the same name is ignored. GITHUB/REF may be
# omitted once a pin exists. THIRD_PARTY skips step 2 (a stray ../curl must not
# replace the pinned curl). SOURCE_ONLY fetches without add_subdirectory().
# OPTIONS are cache entries forced before the dependency is added. Afterwards
# <name>_SOURCE_DIR holds the source tree. Keep each call's name, GITHUB and REF
# on one line: scripts/bump-deps.sh in bro rewrites pins in place.
include_guard(GLOBAL)
include(FetchContent)

function(bro_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 A "THIRD_PARTY;PIN_ONLY;SOURCE_ONLY" "GITHUB;REF;TARGET" "OPTIONS")
    string(TOLOWER "${name}" lc)
    string(TOUPPER "${name}" uc)
    set(P _bro_dep_${lc})
    get_property(url GLOBAL PROPERTY ${P}_url)
    if(NOT url)
        if(NOT A_GITHUB OR NOT A_REF)
            message(FATAL_ERROR "bro_dependency(${name}): no pin; pass GITHUB <owner/repo> REF <sha>")
        endif()
        set(url "https://github.com/${A_GITHUB}/archive/${A_REF}.tar.gz")
        set_property(GLOBAL PROPERTY ${P}_url "${url}")
        set_property(GLOBAL PROPERTY ${P}_third_party ${A_THIRD_PARTY})
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
        get_property(third_party GLOBAL PROPERTY ${P}_third_party)
        if(NOT third_party AND NOT FETCHCONTENT_SOURCE_DIR_${uc})
            foreach(root "${CMAKE_SOURCE_DIR}/.." "${PROJECT_SOURCE_DIR}/..")
                get_filename_component(dir "${root}/${name}" ABSOLUTE)
                if(EXISTS "${dir}/CMakeLists.txt")
                    set(FETCHCONTENT_SOURCE_DIR_${uc} "${dir}")
                    break()
                endif()
            endforeach()
        endif()
        # SOURCE_SUBDIR names nothing, so FetchContent only populates and the
        # add_subdirectory() below decides EXCLUDE_FROM_ALL and the binary dir.
        # Extraction-time timestamps, so a changed pin rebuilds what it changed.
        FetchContent_Declare(${lc} URL "${url}" DOWNLOAD_EXTRACT_TIMESTAMP FALSE
                             SOURCE_SUBDIR _bro_deps_populate_only)
        FetchContent_MakeAvailable(${lc})
        FetchContent_GetProperties(${lc} SOURCE_DIR src)
        set_property(GLOBAL PROPERTY ${P}_src "${src}")
        if(FETCHCONTENT_SOURCE_DIR_${uc})
            message(STATUS "${name}: working tree ${src}")
        else()
            message(STATUS "${name}: ${url}")
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
    add_subdirectory("${src}" "${FETCHCONTENT_BASE_DIR}/${lc}-build" EXCLUDE_FROM_ALL)
endfunction()
