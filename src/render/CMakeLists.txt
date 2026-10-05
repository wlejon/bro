# --- Bidi (UAX #9) ---
#
# Its own target for one reason: bidi.cpp is the only file in bro that includes
# an ICU header, and ICU's headers only declare the symbols the vendored subset
# actually exports when a specific set of renaming macros is defined. Confining
# both the include path and those macros to a one-file target keeps them out of
# every other translation unit, where they would be inert at best and confusing
# at worst.
add_library(bro_bidi STATIC bidi.cpp)
target_include_directories(bro_bidi PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_bidi PUBLIC bro_util)
if(BRO_WITH_TEXT_SHAPING)
    target_link_libraries(bro_bidi PRIVATE skia_icu_bidi_api)
    target_compile_definitions(bro_bidi PRIVATE BRO_WITH_ICU_BIDI=1)
else()
    target_compile_definitions(bro_bidi PRIVATE BRO_WITH_ICU_BIDI=0)
endif()

# --- GLSL -> SPIR-V ---
#
# The in-process glslang wrapper (glsl_compiler.h) on its own, so the build
# tool that compiles bro's built-in shaders links nothing but glslang.
add_library(bro_glsl STATIC glsl_compiler.cpp)
target_include_directories(bro_glsl PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_glsl PRIVATE glslang::glslang glslang::glslang-default-resource-limits)
target_compile_features(bro_glsl PUBLIC cxx_std_20)

add_executable(bro_spirv_embed tools/spirv_embed.cpp)
target_link_libraries(bro_spirv_embed PRIVATE bro_glsl)

set(RENDER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_spv")
file(MAKE_DIRECTORY "${RENDER_GEN_DIR}")
set(RENDER_SPV_HEADERS "")
bro_spirv_header(RENDER_SPV_HEADERS "${RENDER_GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present_quad.vert" vert)
bro_spirv_header(RENDER_SPV_HEADERS "${RENDER_GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/present_overlay.frag" frag)

# --- Render library (Vulkan) ---
find_package(Vulkan REQUIRED)

add_library(bro_render STATIC
    skia_backend.cpp
    skia_backend_surfaces.cpp
    skia_backend_gradient.cpp
    skia_gpu.cpp
    skia_persistent_cache.cpp
    vulkan_context.cpp
    vulkan_context_resources.cpp
    vulkan_debug.cpp
    vulkan_frames.cpp
    vulkan_memory_pool.cpp
    vulkan_pipeline_cache.cpp
    vulkan_queue.cpp
    vulkan_swapchain.cpp
    vulkan_presenter.cpp
    vulkan_presenter_blend.cpp
    vulkan_util.cpp
    pixel_convert.cpp
    raster_renderer.cpp
    font_fallback.cpp
    font_family.cpp
    system_font_mgr.cpp
    shaped_run.cpp
    filter_chain.cpp
    recording_renderer.cpp
    command_replayer.cpp
    image_cache.cpp
    ${RENDER_SPV_HEADERS}
)
target_include_directories(bro_render PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_render PRIVATE ${RENDER_GEN_DIR})
target_link_libraries(bro_render PUBLIC bro_platform skia bro_svg bro_util bro_bidi bro_glsl broimage::broimage Vulkan::Vulkan)

# WebP decoding. Its own small library rather than a part of bro_render,
# because bro has two independent image entry points that must agree — the
# renderer's draw path (image_cache.cpp, here) and `new Image()` (bronze_host's
# host_image.cpp) — and neither of those links the other. See
# webp_image.h for the full why, and skia_modules.cmake for why libwebp is
# compiled from the Skia source bundle rather than pulled from vcpkg.
if(BRO_WITH_WEBP)
    add_library(bro_webp STATIC webp_image.cpp)
    target_include_directories(bro_webp PUBLIC ${bro_SOURCE_DIR}/src)
    target_link_libraries(bro_webp PRIVATE webp_decode)
    target_compile_definitions(bro_webp PUBLIC BRO_WITH_WEBP=1)
    target_link_libraries(bro_render PRIVATE bro_webp)
else()
    target_compile_definitions(bro_render PRIVATE BRO_WITH_WEBP=0)
endif()

if(BRO_BUILD_TESTS)
    add_executable(bro_vulkan_test tests/test_vulkan.cpp)
    target_link_libraries(bro_vulkan_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()
