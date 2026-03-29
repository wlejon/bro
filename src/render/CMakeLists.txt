# --- Shader compilation (HLSL → DXBC via fxc) ---
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

find_program(FXC fxc
    HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
          "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64")

if(NOT FXC)
    message(FATAL_ERROR "fxc.exe not found — install the Windows SDK")
endif()

set(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/color.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/color.frag.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture.vert.hlsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/texture.frag.hlsl
)

add_custom_command(
    OUTPUT "${SHADER_OUT_DIR}/compiled_shaders.h"
    COMMAND ${CMAKE_COMMAND}
        "-DFXC=${FXC}"
        "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "-DOUT_DIR=${SHADER_OUT_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compile_shaders.cmake"
    DEPENDS ${SHADER_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compile_shaders.cmake"
    COMMENT "Compiling HLSL shaders"
)

add_custom_target(compile_shaders DEPENDS "${SHADER_OUT_DIR}/compiled_shaders.h")

# --- Render library ---
add_library(bro_render STATIC skia_backend.cpp gpu_context.cpp)
add_dependencies(bro_render compile_shaders)
target_include_directories(bro_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_render PRIVATE ${SHADER_OUT_DIR})
target_link_libraries(bro_render PUBLIC bro_platform skia bro_util)
