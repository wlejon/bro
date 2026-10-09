# SDL is one platform backend, private to src/platform (src/platform/README.md).
#
# bro_sdl_fence_dir(<out-var>) builds a directory that shadows every SDL3
# header with one that stops the compile, and returns its path. src/ puts it
# first on the include path of every target it defines; src/platform takes it
# off again. An `#include <SDL3/...>` anywhere else in bro then fails with a
# message pointing at the platform interfaces, even though SDL's own include
# directory still reaches those targets transitively (broaudio links SDL
# publicly for its audio devices).

function(bro_sdl_fence_dir out_var)
    # SDL's library targets get their headers from an interface target
    # (SDL3::Headers); a prebuilt/imported SDL carries them itself.
    set(_dirs "")
    foreach(_t SDL3::Headers SDL3_Headers SDL3::SDL3 SDL3::SDL3-static SDL3-static SDL3-shared)
        if(NOT TARGET ${_t})
            continue()
        endif()
        set(_real ${_t})
        get_target_property(_aliased ${_t} ALIASED_TARGET)
        if(_aliased)
            set(_real ${_aliased})
        endif()
        get_target_property(_d ${_real} INTERFACE_INCLUDE_DIRECTORIES)
        if(_d)
            list(APPEND _dirs ${_d})
        endif()
    endforeach()

    set(_headers "")
    foreach(_d IN LISTS _dirs)
        if(_d MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
            set(_d "${CMAKE_MATCH_1}")
        elseif(_d MATCHES "^\\$<")
            continue()  # install-interface paths: not this build's headers
        endif()
        file(GLOB _found RELATIVE "${_d}" "${_d}/SDL3/*.h")
        list(APPEND _headers ${_found})
    endforeach()
    list(REMOVE_DUPLICATES _headers)
    if(NOT _headers)
        message(FATAL_ERROR "bro: found no SDL3 headers to fence (include dirs: ${_dirs})")
    endif()

    set(_fence "${CMAKE_BINARY_DIR}/bro_sdl_fence")
    foreach(_h IN LISTS _headers)
        set(_text "#error \"SDL is private to src/platform: use the interfaces in src/platform (window.h, event_loop.h, keys.h, ...); see src/platform/README.md\"\n")
        set(_path "${_fence}/${_h}")
        if(EXISTS "${_path}")
            file(READ "${_path}" _old)
            if(_old STREQUAL _text)
                continue()
            endif()
        endif()
        file(WRITE "${_path}" "${_text}")
    endforeach()
    set(${out_var} "${_fence}" PARENT_SCOPE)
endfunction()
