set(EMBED_SCRIPT "${bro_SOURCE_DIR}/cmake/embed_js.cmake")

## ── Vulkan brokernel shader embedding ────────────────────────────────────────
# Every built-in 3D scene shader is written in brokernel (.bk) under
# vulkan/shaders/ and compiled in-process by brass to SPIR-V. Each .bk file is
# embedded as a string literal (<file>.src.h); BRO_SCENE_SHADER_SOURCE=1 reads
# them from this tree at run time instead, with no rebuild.
set(VK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_vk")
file(MAKE_DIRECTORY "${VK_GEN_DIR}")
set(VK_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders")

set(VK_GENERATED_HEADERS "")
set_source_files_properties(vulkan/draw/scene_vk_kernel_hook.cpp PROPERTIES
    COMPILE_DEFINITIONS "BRO_SCENE_SHADER_SOURCE_DIR=\"${VK_SHADER_DIR}\"")

function(embed_vk_shader SHADER_FILE VAR_NAME)
    get_filename_component(_name "${SHADER_FILE}" NAME)
    set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/${SHADER_FILE}")
    set(OUTPUT "${VK_GEN_DIR}/${_name}.src.h")
    add_custom_command(
        OUTPUT  "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${INPUT}"
                -DOUTPUT="${OUTPUT}"
                -DVAR_NAME="${VAR_NAME}"
                -P "${EMBED_SCRIPT}"
        DEPENDS "${INPUT}" "${EMBED_SCRIPT}"
        COMMENT "Embedding vulkan/shaders/${SHADER_FILE} -> ${_name}.src.h"
    )
    list(APPEND VK_GENERATED_HEADERS "${OUTPUT}")
    set(VK_GENERATED_HEADERS "${VK_GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

embed_vk_shader(common/scene.bk                 kBkSceneSrc)
embed_vk_shader(common/atmosphere.bk            kBkAtmosphereSrc)
embed_vk_shader(common/lighting.bk              kBkLightingSrc)
embed_vk_shader(common/custom.bk                kBkCustomSrc)
embed_vk_shader(common/vegetation.bk            kBkVegetationSrc)
embed_vk_shader(common/procedural.bk            kBkProceduralSrc)
embed_vk_shader(draw/bark.bk                    kBkBarkSrc)
embed_vk_shader(draw/mesh.bk                    kBkMeshSrc)
embed_vk_shader(draw/mesh_main.bk               kBkMeshMainSrc)
embed_vk_shader(shadow/shadow.bk                kBkShadowSrc)
embed_vk_shader(shadow/shadow_mark.bk           kBkShadowMarkSrc)
embed_vk_shader(lighting/atmosphere_tables.bk   kBkAtmosphereTablesSrc)
embed_vk_shader(lighting/ibl.bk                 kBkIblSrc)
embed_vk_shader(lighting/sky.bk                 kBkSkySrc)
embed_vk_shader(post/postfx.bk                  kBkPostFxSrc)
embed_vk_shader(post/exposure.bk                kBkExposureSrc)
embed_vk_shader(draw/billboard.bk               kBkBillboardSrc)
embed_vk_shader(draw/decal.bk                   kBkDecalSrc)
embed_vk_shader(draw/gaussian_splat.bk          kBkGaussianSplatSrc)
embed_vk_shader(draw/particles.bk               kBkParticlesSrc)
embed_vk_shader(draw/volume.bk                  kBkVolumeSrc)
embed_vk_shader(draw/volume_main.bk             kBkVolumeMainSrc)
embed_vk_shader(draw/volume_near.bk             kBkVolumeNearSrc)
embed_vk_shader(draw/near_grid.bk               kBkNearGridSrc)
embed_vk_shader(draw/near_none.bk               kBkNearNoneSrc)

## ── Library ──────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    scene_renderer.cpp
    scene_renderer_shadow.cpp
    debug_views.cpp
    animation_player.cpp
    animation_player_state_machine.cpp
    animation_player_root_motion.cpp
    tween.cpp
    clip_player.cpp
    sdf_gpu_tracer.cpp
    sdf_gpu_vulkan.cpp
    agent_binding.cpp
    ai_world_ticker.cpp
    nodes/shape_node.cpp
    nodes/sprite_node.cpp
    nodes/physics_node.cpp
    nodes/mesh_node.cpp
    nodes/skinned_mesh_node.cpp
    nodes/instanced_mesh_node.cpp
    nodes/gaussian_splat_node.cpp
    nodes/html_node.cpp
    nodes/light_node.cpp
    nodes/particle_node.cpp
    nodes/particles3d_node.cpp
    nodes/decal_node.cpp
    nodes/reflection_probe_node.cpp
    nodes/terrain_manager.cpp
    nodes/terrain_manager_mesh.cpp
    nodes/terrain_manager_raycast.cpp
    nodes/tile_world.cpp
    nodes/tile_world_mesh.cpp
    nodes/tile_world_objects.cpp
    world/world_service.cpp
    world/shadow_pages.cpp
    world/shadow_pages_plan.cpp
    vulkan/frame/scene_vk_device.cpp
    vulkan/frame/scene_vk_allocator.cpp
    vulkan/frame/scene_vk_pipeline.cpp
    vulkan/frame/scene_shader_sets.cpp
    vulkan/frame/scene_vk_descriptors.cpp
    vulkan/frame/scene_view.cpp
    vulkan/frame/scene_inspect_record.cpp
    vulkan/frame/scene_view_inspect.cpp
    vulkan/frame/scene_defaults.cpp
    vulkan/frame/scene_targets.cpp
    vulkan/frame/scene_gpu_timer.cpp
    vulkan/frame/scene_gpu_resources.cpp
    vulkan/frame/scene_frame_graph.cpp
    vulkan/frame/scene_fullscreen.cpp
    vulkan/frame/pass_snapshot.cpp
    vulkan/frame/scene_vk_bridge.cpp
    vulkan/world/scene_world_service.cpp
    vulkan/world/pass_world_service.cpp
    vulkan/draw/scene_vk_custom_shader.cpp
    vulkan/draw/scene_vk_kernel_hook.cpp
    vulkan/draw/scene_mesh_drawer.cpp
    vulkan/draw/scene_draw_list.cpp
    vulkan/draw/pass_mesh.cpp
    vulkan/draw/pass_decal.cpp
    vulkan/draw/pass_volume.cpp
    vulkan/draw/pass_particles.cpp
    vulkan/draw/pass_billboard.cpp
    vulkan/draw/pass_gaussian_splat.cpp
    vulkan/lighting/scene_lighting.cpp
    vulkan/lighting/scene_environment.cpp
    vulkan/lighting/scene_atmosphere.cpp
    vulkan/lighting/pass_atmosphere.cpp
    vulkan/lighting/pass_environment.cpp
    vulkan/lighting/pass_reflection_probe.cpp
    vulkan/shadow/pass_shadow.cpp
    vulkan/shadow/scene_caster_drawer.cpp
    vulkan/shadow/pass_shadow_copy.cpp
    vulkan/shadow/pass_shadow_mark.cpp
    vulkan/shadow/scene_shadow_pages.cpp
    vulkan/post/pass_ssao.cpp
    vulkan/post/pass_ssr.cpp
    vulkan/post/pass_dof.cpp
    vulkan/post/scene_exposure.cpp
    vulkan/post/pass_postfx.cpp
    ${VK_GENERATED_HEADERS}
)

target_include_directories(bro_scene PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_scene PRIVATE ${VK_GEN_DIR})
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here, as is brokernel (3D forces KERNEL on).
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh FastNoise brogameagent bro_dom bro_render bro_layout bro_tile Vulkan::Vulkan)
target_link_libraries(bro_scene PRIVATE brokernel)
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

    # The world service's policy (ranking, budget from slack, cost learning),
    # CPU only.
    add_executable(bro_world_service_test tests/test_world_service.cpp)
    target_link_libraries(bro_world_service_test PRIVATE bro_scene bro_render bro_platform bro_util)
    target_include_directories(bro_world_service_test PRIVATE ${bro_SOURCE_DIR}/src)

    # The cached shadow pages' paging policy (addressing, snapping, requests,
    # wants and grants, the pool's reuse, invalidation, composites) and the
    # casters' still and moving states, CPU only: its sources and bromath.
    add_executable(bro_shadow_pages_test tests/test_shadow_pages.cpp world/shadow_pages.cpp
                   world/shadow_pages_plan.cpp)
    target_link_libraries(bro_shadow_pages_test PRIVATE bromath)
    target_include_directories(bro_shadow_pages_test PRIVATE ${bro_SOURCE_DIR}/src)

    # The atmosphere's tables against a CPU reference march (single
    # scattering brute force, multiple scattering by iterated orders).
    add_executable(bro_atmosphere_test tests/test_atmosphere.cpp tests/atmosphere_reference.cpp)
    target_link_libraries(bro_atmosphere_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_atmosphere_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})

    # Live GPU SDF sphere-tracing, cone-traced soft shadows, ambient occlusion,
    # and SPIR-V 1.5 compute shader formal verification.
    add_executable(bro_sdf_gpu_tracer_test tests/test_sdf_gpu_tracer.cpp)
    target_link_libraries(bro_sdf_gpu_tracer_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util bro_glsl bromesh bromath brass skia Vulkan::Vulkan)
    target_include_directories(bro_sdf_gpu_tracer_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})
endif()
