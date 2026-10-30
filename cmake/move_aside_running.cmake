# Run as a pre-link step on Windows: cmake -DFILE=<binary> -P move_aside_running.cmake
#
# Windows refuses to overwrite an executable or DLL a running process has
# mapped (LNK1104), and a bro is usually running from the build tree (the
# user's window, another session's tests). It does allow renaming one: the
# process keeps its mapping under the new name. So the old binary is moved
# aside to <binary>.stale-<n> and the linker writes a fresh one, and every
# stale copy no running process still holds is deleted here, including the
# one just made when nothing held it.

if(NOT FILE)
    message(FATAL_ERROR "move_aside_running.cmake: FILE is not set")
endif()

if(EXISTS "${FILE}")
    string(TIMESTAMP stamp "%Y%m%d%H%M%S")
    string(RANDOM LENGTH 6 ALPHABET 0123456789 salt)
    file(RENAME "${FILE}" "${FILE}.stale-${stamp}${salt}" RESULT renamed)
    if(NOT renamed EQUAL 0)
        message(WARNING "could not move ${FILE} aside: ${renamed}")
    endif()
endif()

file(GLOB stale "${FILE}.stale-*")
foreach(f IN LISTS stale)
    # A copy still mapped by a process fails to delete and is left for the
    # next link; rm -f keeps that failure quiet.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${f}"
                    OUTPUT_QUIET ERROR_QUIET)
endforeach()
