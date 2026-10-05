find_package(Vulkan REQUIRED)

add_library(bro_webgl STATIC
    webgl2_context.cpp
    webgl2_context_shaders.cpp
    webgl2_context_textures.cpp
    webgl2_context_render.cpp
    vulkan/webgl_vk_canvas.cpp
    vulkan/webgl_vk_glsl.cpp
    vulkan/webgl_vk_spirv.cpp
    vulkan/webgl_vk_pipeline.cpp
    vulkan/webgl_vk_context.cpp
    vulkan/webgl_vk_context_buffers.cpp
    vulkan/webgl_vk_context_program.cpp
    vulkan/webgl_vk_context_introspection.cpp
    vulkan/webgl_vk_context_placeholders.cpp
    vulkan/webgl_vk_context_uniforms.cpp
    vulkan/webgl_vk_context_textures.cpp
    vulkan/webgl_vk_context_texstorage.cpp
    vulkan/webgl_vk_context_texcopy.cpp
    vulkan/webgl_vk_context_samplers.cpp
    vulkan/webgl_vk_context_framebuffers.cpp
    vulkan/webgl_vk_context_renderbuffers.cpp
    vulkan/webgl_vk_context_drawcalls.cpp
    vulkan/webgl_vk_context_vertices.cpp
    vulkan/webgl_vk_context_maskedclear.cpp
    vulkan/webgl_vk_context_queries.cpp
    vulkan/webgl_vk_context_feedback.cpp
    vulkan/webgl_vk_context_getters.cpp
    vulkan/webgl_vk_context_blit.cpp
    vulkan/webgl_vk_context_clear.cpp
    vulkan/webgl_vk_context_readback.cpp
    vulkan/webgl_vk_context_parameters.cpp
    vulkan/webgl_vk_formats.cpp
    vulkan/webgl_vk_texformat.cpp
    vulkan/webgl_vk_pixels.cpp
    vulkan/webgl_vk_context_commands.cpp
    vulkan/webgl_vk_context_draw.cpp
    vulkan/webgl_vk_stream.cpp
)
target_include_directories(bro_webgl PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_webgl PUBLIC bro_render bro_util Vulkan::Vulkan)
# Linking a WebGL program uses glslang's C++ API for its reflection.
target_link_libraries(bro_webgl PRIVATE bro_glsl glslang::glslang glslang::glslang-default-resource-limits)

if(BRO_BUILD_TESTS)
    add_executable(bro_vulkan_webgl_test tests/test_webgl_vulkan.cpp)
    target_link_libraries(bro_vulkan_webgl_test PRIVATE bro_webgl bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_webgl_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()
