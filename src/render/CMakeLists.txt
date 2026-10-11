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
    shared_pixels_image.cpp
    skia_backend_gradient.cpp
    shadow_ninepatch.cpp
    skia_gpu.cpp
    skia_gpu_upload.cpp
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
    pci_link.cpp
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
    image_store.cpp
    image_source.cpp
    image_codecs.cpp
    animated_image.cpp
    software_cursor.cpp
    remote_picture.cpp
    ${RENDER_SPV_HEADERS}
)
if(WIN32)
    # The remote viewer's zero-copy path: decoder D3D11 pictures into Vulkan.
    target_sources(bro_render PRIVATE remote_picture_d3d11.cpp)
    target_link_libraries(bro_render PRIVATE d3d11 dxgi setupapi)
endif()
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
    target_link_libraries(bro_webp PRIVATE webp_decode broimage::broimage)
    target_compile_definitions(bro_webp PUBLIC BRO_WITH_WEBP=1)
    target_link_libraries(bro_render PRIVATE bro_webp)
else()
    target_compile_definitions(bro_render PRIVATE BRO_WITH_WEBP=0)
endif()

# AVIF's AV1 decode: dav1d (BSD-2) from vcpkg, registered with broimage,
# which reads the HEIF container and converts the YUV itself. vcpkg's dav1d
# is a meson build with no CMake package: found by header and library.
if(BRO_WITH_AVIF)
    find_path(DAV1D_INCLUDE_DIR dav1d/dav1d.h REQUIRED)
    find_library(DAV1D_LIBRARY_RELEASE NAMES dav1d libdav1d REQUIRED)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        find_library(DAV1D_LIBRARY_DEBUG NAMES dav1d libdav1d
                     PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
    endif()
    add_library(bro_avif STATIC avif_av1.cpp)
    target_include_directories(bro_avif PUBLIC ${bro_SOURCE_DIR}/src)
    target_include_directories(bro_avif PRIVATE ${DAV1D_INCLUDE_DIR})
    if(DAV1D_LIBRARY_DEBUG)
        target_link_libraries(bro_avif PRIVATE optimized ${DAV1D_LIBRARY_RELEASE} debug ${DAV1D_LIBRARY_DEBUG})
    else()
        target_link_libraries(bro_avif PRIVATE ${DAV1D_LIBRARY_RELEASE})
    endif()
    target_link_libraries(bro_avif PRIVATE broimage::broimage)
    target_compile_definitions(bro_render PRIVATE BRO_WITH_AVIF=1)
    target_link_libraries(bro_render PRIVATE bro_avif)
else()
    target_compile_definitions(bro_render PRIVATE BRO_WITH_AVIF=0)
endif()

if(BRO_WITH_DMABUF)
    target_sources(bro_render PRIVATE
        vulkan_dmabuf_importer.cpp
        kms_direct_presenter.cpp
        scanout_capture.cpp
    )
    target_link_libraries(bro_render PUBLIC brodmabuf::brodmabuf)
    target_compile_definitions(bro_render PUBLIC BRO_WITH_DMABUF=1)
else()
    target_compile_definitions(bro_render PUBLIC BRO_WITH_DMABUF=0)
endif()

if(BRO_BUILD_TESTS)
    add_executable(bro_vulkan_test tests/test_vulkan.cpp)
    target_link_libraries(bro_vulkan_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_software_cursor_test tests/test_software_cursor.cpp)
    target_link_libraries(bro_software_cursor_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_software_cursor_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_shadow_ninepatch_test tests/test_shadow_ninepatch.cpp)
    target_link_libraries(bro_shadow_ninepatch_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_shadow_ninepatch_test PRIVATE ${bro_SOURCE_DIR}/src)

    add_executable(bro_pci_link_test tests/test_pci_link.cpp)
    target_link_libraries(bro_pci_link_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_pci_link_test PRIVATE ${bro_SOURCE_DIR}/src)

    if(BRO_WITH_DMABUF)
        add_executable(bro_vulkan_dmabuf_test tests/test_dmabuf_presenter.cpp)
        target_link_libraries(bro_vulkan_dmabuf_test PRIVATE bro_render bro_platform bro_util skia Vulkan::Vulkan brodmabuf::brodmabuf)
        target_include_directories(bro_vulkan_dmabuf_test PRIVATE ${bro_SOURCE_DIR}/src)
    endif()
endif()
