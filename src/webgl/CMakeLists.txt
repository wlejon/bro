add_library(bro_webgl STATIC
    webgl2_context.cpp
    glsl_translator.cpp
)
target_include_directories(bro_webgl PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_webgl PUBLIC bro_render glad bro_util)
