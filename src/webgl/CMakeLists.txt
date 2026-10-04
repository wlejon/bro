find_package(Vulkan REQUIRED)

add_library(bro_webgl STATIC
    webgl2_context.cpp
    webgl2_context_shaders.cpp
    webgl2_context_textures.cpp
    webgl2_context_render.cpp
    glsl_translator.cpp
    vulkan/webgl_vk_canvas.cpp
    vulkan/webgl_vk_shaders_parser.cpp
    vulkan/webgl_vk_shaders.cpp
    vulkan/webgl_vk_pipeline.cpp
    vulkan/webgl_vk_context.cpp
    vulkan/webgl_vk_context_buffers.cpp
    vulkan/webgl_vk_context_uniforms.cpp
    vulkan/webgl_vk_context_textures.cpp
    vulkan/webgl_vk_context_fbo.cpp
    vulkan/webgl_vk_context_draw.cpp
)
target_include_directories(bro_webgl PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_webgl PUBLIC bro_render bro_util Vulkan::Vulkan)

if(BRO_BUILD_TESTS)
    add_executable(bro_vulkan_webgl_test tests/test_webgl_vulkan.cpp)
    target_link_libraries(bro_vulkan_webgl_test PRIVATE bro_webgl bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_webgl_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()
