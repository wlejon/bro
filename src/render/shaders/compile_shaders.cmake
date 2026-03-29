# compile_shaders.cmake — Compile HLSL to DXBC and generate C headers
#
# Called as a CMake script: cmake -P compile_shaders.cmake
#   -DFXC=<path>  -DSRC_DIR=<dir>  -DOUT_DIR=<dir>
#
# Generates compiled_shaders.h in OUT_DIR with byte arrays for each shader.

set(SHADER_FILES   color.vert.hlsl   color.frag.hlsl   texture.vert.hlsl   texture.frag.hlsl)
set(SHADER_PROFILES vs_5_1           ps_5_1            vs_5_1              ps_5_1)
set(SHADER_NAMES    color_vert       color_frag        texture_vert        texture_frag)

list(LENGTH SHADER_FILES SHADER_COUNT)
math(EXPR SHADER_LAST "${SHADER_COUNT} - 1")

file(MAKE_DIRECTORY "${OUT_DIR}")

set(HEADER_CONTENT "#pragma once\n#include <cstdint>\n#include <cstddef>\n\n")
string(APPEND HEADER_CONTENT "// Auto-generated from HLSL — do not edit\n\n")
string(APPEND HEADER_CONTENT "namespace bro::render::shaders {\n\n")

foreach(idx RANGE ${SHADER_LAST})
    list(GET SHADER_FILES ${idx} HLSL_FILE)
    list(GET SHADER_PROFILES ${idx} PROFILE)
    list(GET SHADER_NAMES ${idx} VAR_NAME)

    set(SRC "${SRC_DIR}/${HLSL_FILE}")
    set(CSO "${OUT_DIR}/${VAR_NAME}.cso")

    # Compile HLSL → DXBC
    execute_process(
        COMMAND "${FXC}" /nologo /T ${PROFILE} /E main /O2 /Fo "${CSO}" "${SRC}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE fxc_out
        ERROR_VARIABLE fxc_err
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "fxc failed for ${HLSL_FILE}:\n${fxc_err}")
    endif()

    # Read binary and convert to C array
    file(READ "${CSO}" BYTES HEX)
    string(LENGTH "${BYTES}" HEX_LEN)
    math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

    set(ARRAY_BODY "")
    set(i 0)
    while(i LESS HEX_LEN)
        math(EXPR end "${i} + 2")
        string(SUBSTRING "${BYTES}" ${i} 2 byte)
        if(NOT "${ARRAY_BODY}" STREQUAL "")
            string(APPEND ARRAY_BODY ",")
        endif()
        math(EXPR col "(${i} / 2) % 16")
        if(col EQUAL 0)
            string(APPEND ARRAY_BODY "\n    ")
        endif()
        string(APPEND ARRAY_BODY "0x${byte}")
        math(EXPR i "${i} + 2")
    endwhile()

    string(APPEND HEADER_CONTENT "inline const uint8_t ${VAR_NAME}[] = {${ARRAY_BODY}\n};\n")
    string(APPEND HEADER_CONTENT "inline const size_t ${VAR_NAME}_size = ${BYTE_COUNT};\n\n")
endforeach()

string(APPEND HEADER_CONTENT "} // namespace bro::render::shaders\n")

file(WRITE "${OUT_DIR}/compiled_shaders.h" "${HEADER_CONTENT}")
message(STATUS "Generated compiled_shaders.h (${OUT_DIR})")
