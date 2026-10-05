set(EMBED_SCRIPT "${bro_SOURCE_DIR}/cmake/embed_js.cmake")


## ── Vulkan SPIR-V shader compilation ──────────────────────────────────────────
# The built-in scene shaders, compiled at build time by the same in-process
# glslang the engine uses at run time (bro_spirv_header, cmake/glslang.cmake).
set(VK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_vk")
file(MAKE_DIRECTORY "${VK_GEN_DIR}")

set(VK_GENERATED_HEADERS "")
foreach(_shader
        mesh.vert
        mesh_instanced.vert
        mesh_skinned.vert
        mesh.frag
        shadow.vert
        shadow_instanced.vert
        shadow_skinned.vert
        shadow.frag
        skybox.vert
        environment.frag
        postfx.vert
        tonemap.frag
        bloom.frag
        fxaa.frag
        billboard.vert
        billboard.frag
        particles.vert
        particles.frag
        decal.vert
        decal.frag
        blur.frag
        color_lut.frag
        ssao.frag
        ssr.frag
        dof.frag
        apply_ao.frag
        gaussian_splat.vert
        gaussian_splat.frag
    )
    if(_shader MATCHES "\\.vert$")
        set(_stage vert)
    else()
        set(_stage frag)
    endif()
    bro_spirv_header(VK_GENERATED_HEADERS "${VK_GEN_DIR}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/${_shader}" ${_stage})
endforeach()
# The opaque mesh shader that also writes the indirect light for SSAO.
bro_spirv_header(VK_GENERATED_HEADERS "${VK_GEN_DIR}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/mesh.frag" frag
                 NAME mesh_indirect.frag DEFINES SCENE_INDIRECT_OUTPUT)

function(embed_vk_shader SHADER_FILE VAR_NAME)
    set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/${SHADER_FILE}")
    set(OUTPUT "${VK_GEN_DIR}/${SHADER_FILE}.src.h")
    add_custom_command(
        OUTPUT  "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${INPUT}"
                -DOUTPUT="${OUTPUT}"
                -DVAR_NAME="${VAR_NAME}"
                -P "${EMBED_SCRIPT}"
        DEPENDS "${INPUT}" "${EMBED_SCRIPT}"
        COMMENT "Embedding vulkan/shaders/${SHADER_FILE} -> ${SHADER_FILE}.src.h"
    )
    list(APPEND VK_GENERATED_HEADERS "${OUTPUT}")
    set(VK_GENERATED_HEADERS "${VK_GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

embed_vk_shader(mesh.vert             kVkMeshVertSrc)
embed_vk_shader(mesh.frag             kVkMeshFragSrc)
embed_vk_shader(mesh_instanced.vert   kVkMeshInstancedVertSrc)
embed_vk_shader(mesh_skinned.vert     kVkMeshSkinnedVertSrc)
embed_vk_shader(shadow.vert           kVkShadowVertSrc)
embed_vk_shader(shadow_skinned.vert   kVkShadowSkinnedVertSrc)
embed_vk_shader(clipmap_common.glsl       kClipmapCommonSrc)
embed_vk_shader(clipmap_detail.glsl       kClipmapDetailSrc)
embed_vk_shader(clipmap_material.glsl     kClipmapMaterialSrc)
embed_vk_shader(clipmap_cubic.glsl        kClipmapCubicSrc)
embed_vk_shader(clipmap_cubic_height.glsl kClipmapCubicHeightSrc)
embed_vk_shader(clipmap.vert.glsl         kClipmapVertSrc)
embed_vk_shader(clipmap.frag.glsl         kClipmapFragSrc)

## ── Library ──────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    impostor_layer.cpp
    depth_policy.cpp
    scene_renderer.cpp
    scene_renderer_shadow.cpp
    shape_node.cpp
    sprite_node.cpp
    physics_node.cpp
    mesh_node.cpp
    skinned_mesh_node.cpp
    animation_player.cpp
    tween.cpp
    clip_player.cpp
    instanced_mesh_node.cpp
    gaussian_splat_node.cpp
    html_node.cpp
    light_node.cpp
    particle_node.cpp
    particles3d_node.cpp
    decal_node.cpp
    reflection_probe_node.cpp
    terrain_manager.cpp
    clipmap_terrain.cpp
    clipmap_terrain_elevation.cpp
    tile_world.cpp
    tile_world_mesh.cpp
    tile_world_objects.cpp
    agent_binding.cpp
    ai_world_ticker.cpp
    vulkan/scene_vk_device.cpp
    vulkan/scene_vk_allocator.cpp
    vulkan/scene_vk_pipeline.cpp
    vulkan/scene_vk_descriptors.cpp
    vulkan/scene_vk_shader_compiler.cpp
    vulkan/scene_vk_custom_shader.cpp
    vulkan/scene_view.cpp
    vulkan/scene_defaults.cpp
    vulkan/scene_targets.cpp
    vulkan/scene_gpu_resources.cpp
    vulkan/scene_mesh_drawer.cpp
    vulkan/scene_draw_list.cpp
    vulkan/scene_frame_graph.cpp
    vulkan/scene_fullscreen.cpp
    vulkan/scene_lighting.cpp
    vulkan/pass_shadow.cpp
    vulkan/pass_reflection_probe.cpp
    vulkan/pass_environment.cpp
    vulkan/pass_mesh.cpp
    vulkan/pass_terrain.cpp
    vulkan/pass_snapshot.cpp
    vulkan/pass_ssao.cpp
    vulkan/pass_ssr.cpp
    vulkan/pass_decal.cpp
    vulkan/pass_particles.cpp
    vulkan/pass_billboard.cpp
    vulkan/pass_gaussian_splat.cpp
    vulkan/pass_dof.cpp
    vulkan/pass_postfx.cpp
    vulkan/pass_color_lut.cpp
    vulkan/scene_vk_bridge.cpp
    ${VK_GENERATED_HEADERS}
)

target_include_directories(bro_scene PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_scene PRIVATE ${VK_GEN_DIR})
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here.
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh FastNoise brogameagent bro_dom bro_render bro_layout bro_tile Vulkan::Vulkan)
target_compile_features(bro_scene PUBLIC cxx_std_20)
bro_apply_feature_defs(bro_scene)

if(MSVC)
    target_compile_options(bro_scene PRIVATE /MP /permissive- /utf-8)
endif()

if(BRO_BUILD_TESTS)
    add_executable(bro_vulkan_scene_test tests/test_vulkan_scene_core.cpp)
    target_link_libraries(bro_vulkan_scene_test PRIVATE bro_scene bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_scene_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})

    add_executable(bro_vulkan_scene_passes_test tests/test_vulkan_scene_passes.cpp)
    # A real SceneGraph pulls in the DOM, whose style code calls back into the
    # engine's animation managers, so bro_engine is named again after bro_dom.
    target_link_libraries(bro_vulkan_scene_passes_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_scene_passes_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})
endif()


