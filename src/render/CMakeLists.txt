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

# --- Render library (OpenGL and Vulkan backends) ---
find_package(Vulkan REQUIRED)

add_library(bro_render STATIC
    skia_backend.cpp
    skia_backend_gpu.cpp
    skia_backend_gradient.cpp
    vulkan_context.cpp
    vulkan_debug.cpp
    vulkan_memory_pool.cpp
    vulkan_swapchain.cpp
    vulkan_presenter.cpp
    vulkan_presenter_composite.cpp
    raster_renderer.cpp
    font_fallback.cpp
    font_family.cpp
    system_font_mgr.cpp
    shaped_run.cpp
    filter_chain.cpp
    recording_renderer.cpp
    command_replayer.cpp
    image_cache.cpp
)
target_include_directories(bro_render PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_render PUBLIC bro_platform skia bro_svg bro_util bro_bidi broimage::broimage Vulkan::Vulkan)

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
