set(EMBED_SCRIPT "${bro_SOURCE_DIR}/cmake/embed_js.cmake")


## ── Vulkan SPIR-V shader compilation ──────────────────────────────────────────
# The built-in scene shaders, compiled at build time by the same in-process
# glslang the engine uses at run time (bro_spirv_header, cmake/glslang.cmake).
# vulkan/shaders/builtin_shaders.def lists them, with their variants' DEFINES;
# each becomes a <name>.spv.cpp of its own, rebuilt only when the files it
# actually #includes change (the tool's depfile), and the Visual Studio build
# runs them in parallel (CMP0147). BRO_SCENE_SHADER_SOURCE compiles them from
# this tree at run time instead, with no rebuild (docs/hot-reload.md).
if(POLICY CMP0147)
    cmake_policy(SET CMP0147 NEW)
endif()
set(VK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_vk")
file(MAKE_DIRECTORY "${VK_GEN_DIR}")
set(VK_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders")

set(VK_GENERATED_HEADERS "")
set(VK_SHADER_LIST "${VK_SHADER_DIR}/builtin_shaders.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VK_SHADER_LIST}")
file(STRINGS "${VK_SHADER_LIST}" _vkShaderLines REGEX "^BRO_SCENE_SHADER\\(")
foreach(_line IN LISTS _vkShaderLines)
    if(NOT _line MATCHES "^BRO_SCENE_SHADER\\(([A-Za-z0-9_]+), *\"([^\"]+)\", *\"([^\"]+)\", *\"(vert|frag|comp)\", *\"([^\"]*)\"\\)")
        message(FATAL_ERROR "builtin_shaders.def: cannot read the line: ${_line}")
    endif()
    set(_vkEnum "${CMAKE_MATCH_1}")
    set(_vkName "${CMAKE_MATCH_2}")
    set(_vkSource "${CMAKE_MATCH_3}")
    set(_vkStage "${CMAKE_MATCH_4}")
    string(REPLACE " " ";" _vkDefines "${CMAKE_MATCH_5}")
    bro_spirv_header(VK_GENERATED_HEADERS "${VK_GEN_DIR}" "${VK_SHADER_DIR}/${_vkSource}" ${_vkStage}
                     NAME "${_vkName}" SYMBOL "bro::scene::vk::spv::kSpv${_vkEnum}"
                     INCLUDE_DIR "${VK_SHADER_DIR}" DEFINES ${_vkDefines})
endforeach()
set_source_files_properties(vulkan/frame/scene_vk_shader_compiler.cpp PROPERTIES
    COMPILE_DEFINITIONS "BRO_SCENE_SHADER_SOURCE_DIR=\"${VK_SHADER_DIR}\"")

# A shader source embedded as text, for the shaders compiled at run time
# (custom mesh shaders, the world modules' sets). SHADER_FILE is its path under
# vulkan/shaders; the header is named by the file alone (<file>.src.h).
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

embed_vk_shader(draw/mesh.vert                  kVkMeshVertSrc)
embed_vk_shader(draw/mesh.frag                  kVkMeshFragSrc)
embed_vk_shader(draw/mesh_instanced.vert        kVkMeshInstancedVertSrc)
embed_vk_shader(draw/mesh_skinned.vert          kVkMeshSkinnedVertSrc)
embed_vk_shader(shadow/shadow.vert              kVkShadowVertSrc)
embed_vk_shader(shadow/shadow_skinned.vert      kVkShadowSkinnedVertSrc)
embed_vk_shader(shadow/shadow_instanced.vert    kVkShadowInstancedVertSrc)
embed_vk_shader(common/scene_camera.glsl        kVkSceneCameraSrc)
embed_vk_shader(common/scene_lighting.glsl      kVkSceneLightingSrc)
embed_vk_shader(common/scene_atmosphere.glsl    kVkSceneAtmosphereSrc)
embed_vk_shader(common/scene_atmosphere_params.glsl kVkSceneAtmosphereParamsSrc)
embed_vk_shader(lighting/scene_probe_grid.glsl  kVkSceneProbeGridSrc)
embed_vk_shader(lighting/probe_grid_layout.h    kVkProbeGridLayoutSrc)
embed_vk_shader(common/scene_mesh_push.glsl     kVkSceneMeshPushSrc)
embed_vk_shader(common/scene_shadow_push.glsl   kVkSceneShadowPushSrc)
embed_vk_shader(common/scene_shadow_pages.glsl  kVkSceneShadowPagesSrc)
embed_vk_shader(shadow/shadow_pages_layout.h    kVkShadowPagesLayoutSrc)
embed_vk_shader(shadow/scene_shadow_cutout.glsl kVkSceneShadowCutoutSrc)
embed_vk_shader(common/scene_vegetation.glsl    kVkSceneVegetationSrc)
embed_vk_shader(common/scene_cover_mask.glsl    kVkSceneCoverMaskSrc)
embed_vk_shader(common/scene_subpixel_mark.glsl kVkSceneSubpixelMarkSrc)
embed_vk_shader(common/scene_procedural.glsl    kVkSceneProceduralSrc)
embed_vk_shader(draw/scene_bark.glsl            kVkSceneBarkSrc)
embed_vk_shader(ground/scene_grass.glsl         kVkSceneGrassSrc)
embed_vk_shader(ground/scene_ground_noise.glsl  kVkSceneGroundNoiseSrc)
embed_vk_shader(ground/scene_sward_layer.glsl   kVkSceneSwardLayerSrc)
embed_vk_shader(ground/sward_module.glsl        kVkSwardModuleSrc)
embed_vk_shader(ground/scene_ground.glsl        kVkSceneGroundSrc)
embed_vk_shader(ground/scene_ground_layers.glsl kVkSceneGroundLayersSrc)
embed_vk_shader(ground/scene_cover_plan.glsl   kVkSceneCoverPlanSrc)
embed_vk_shader(ground/scene_cover_far.glsl    kVkSceneCoverFarSrc)
embed_vk_shader(ground/scene_herb.glsl         kVkSceneHerbSrc)
embed_vk_shader(ground/scene_herb_layer.glsl   kVkSceneHerbLayerSrc)
embed_vk_shader(ground/herb_module.glsl        kVkHerbModuleSrc)
embed_vk_shader(ground/cover_plan.comp         kVkCoverPlanCompSrc)
embed_vk_shader(ground/mesh_grass.vert         kVkMeshGrassVertSrc)
embed_vk_shader(ground/mesh_herb.vert          kVkMeshHerbVertSrc)
embed_vk_shader(ground/mesh_cover_far.vert     kVkMeshCoverFarVertSrc)
embed_vk_shader(ground/shadow_grass.vert       kVkShadowGrassVertSrc)
embed_vk_shader(ground/ground_layer_fill.frag  kVkGroundLayerFillFragSrc)
embed_vk_shader(ground/ground_chart_fill.vert  kVkGroundChartFillVertSrc)
embed_vk_shader(foliage/scene_plants.glsl      kVkScenePlantsSrc)
embed_vk_shader(foliage/scene_plant_records.glsl kVkScenePlantRecordsSrc)
embed_vk_shader(foliage/scene_plant_layer.glsl kVkScenePlantLayerSrc)
embed_vk_shader(foliage/plant_module.glsl      kVkPlantModuleSrc)
embed_vk_shader(foliage/plants.vert            kVkPlantsVertSrc)
embed_vk_shader(foliage/plants.frag            kVkPlantsFragSrc)
embed_vk_shader(foliage/shadow_plants.vert     kVkShadowPlantsVertSrc)
embed_vk_shader(foliage/plants_pose.comp       kVkPlantsPoseCompSrc)
embed_vk_shader(ground/scene_land_layout.h     kVkSceneLandLayoutSrc)
embed_vk_shader(ground/land_sun_layout.h       kVkLandSunLayoutSrc)
embed_vk_shader(ground/scene_land.glsl         kVkSceneLandSrc)
embed_vk_shader(ground/scene_land_layer.glsl   kVkSceneLandLayerSrc)
embed_vk_shader(ground/land_module.glsl        kVkLandModuleSrc)
embed_vk_shader(ground/land_canopy.glsl        kVkLandCanopySrc)
embed_vk_shader(ground/land_body.glsl          kVkLandBodySrc)
embed_vk_shader(ground/land_trig.glsl          kVkLandTrigSrc)
embed_vk_shader(ground/land_fill.comp          kVkLandFillCompSrc)
embed_vk_shader(ground/land_grid.glsl          kVkLandGridSrc)
embed_vk_shader(ground/land_grid_config.glsl   kVkLandGridConfigSrc)
embed_vk_shader(ground/land_process_layout.h   kVkLandProcessLayoutSrc)
embed_vk_shader(ground/land_process_layer.glsl kVkLandProcessLayerSrc)
embed_vk_shader(ground/land_process_module.glsl kVkLandProcessModuleSrc)
embed_vk_shader(ground/land_process_dispatch.glsl kVkLandProcessDispatchSrc)
embed_vk_shader(ground/land_process.comp       kVkLandProcessCompSrc)
embed_vk_shader(ground/land_regions.glsl       kVkLandRegionsSrc)
embed_vk_shader(ground/land_region_config.glsl kVkLandRegionConfigSrc)
embed_vk_shader(ground/land_region_layer.glsl  kVkLandRegionLayerSrc)
embed_vk_shader(ground/land_region_dispatch.glsl kVkLandRegionDispatchSrc)
embed_vk_shader(ground/land_region.comp        kVkLandRegionCompSrc)
embed_vk_shader(sky/clouds_layout.h            kVkCloudsLayoutSrc)
embed_vk_shader(sky/cloud_field.glsl           kVkCloudFieldSrc)
embed_vk_shader(sky/scene_cloud_layer.glsl     kVkSceneCloudLayerSrc)
embed_vk_shader(sky/cloud_module.glsl          kVkCloudModuleSrc)
embed_vk_shader(sky/clouds_cover.comp          kVkCloudsCoverCompSrc)
embed_vk_shader(sky/clouds_cache.comp          kVkCloudsCacheCompSrc)
embed_vk_shader(sky/clouds_globe.comp          kVkCloudsGlobeCompSrc)

## ── Library ──────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    scene_renderer.cpp
    scene_renderer_shadow.cpp
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
    nodes/grass_node.cpp
    nodes/plants_node.cpp
    nodes/reflection_probe_node.cpp
    nodes/terrain_manager.cpp
    nodes/terrain_manager_mesh.cpp
    nodes/terrain_manager_raycast.cpp
    nodes/land_node.cpp
    nodes/tile_world.cpp
    nodes/tile_world_mesh.cpp
    nodes/tile_world_objects.cpp
    world/world_cells.cpp
    world/surface_layers.cpp
    world/world_service.cpp
    world/shadow_pages.cpp
    world/shadow_pages_plan.cpp
    world/probe_grid.cpp
    world/world_workers.cpp
    world/fields.cpp
    world/cell_store.cpp
    world/plant_placement.cpp
    world/plant_canopy.cpp
    world/land_sun.cpp
    world/place.cpp
    world/body_surface_map.cpp
    world/land_grid.cpp
    world/land_regions.cpp
    world/cover_regions.cpp
    vulkan/frame/scene_vk_device.cpp
    vulkan/frame/scene_vk_allocator.cpp
    vulkan/frame/scene_vk_pipeline.cpp
    vulkan/frame/scene_vk_descriptors.cpp
    vulkan/frame/scene_vk_shader_compiler.cpp
    vulkan/frame/scene_module_shaders.cpp
    vulkan/frame/scene_view.cpp
    vulkan/frame/scene_defaults.cpp
    vulkan/frame/scene_targets.cpp
    vulkan/frame/scene_gpu_timer.cpp
    vulkan/frame/scene_gpu_resources.cpp
    vulkan/frame/scene_frame_graph.cpp
    vulkan/frame/scene_fullscreen.cpp
    vulkan/frame/pass_snapshot.cpp
    vulkan/frame/scene_vk_bridge.cpp
    vulkan/world/scene_surface_layers.cpp
    vulkan/world/scene_world_cells_pass.cpp
    vulkan/world/scene_world_horizon.cpp
    vulkan/world/scene_world_service.cpp
    vulkan/world/pass_world_service.cpp
    vulkan/draw/scene_vk_custom_shader.cpp
    vulkan/draw/scene_mesh_drawer.cpp
    vulkan/draw/scene_draw_list.cpp
    vulkan/draw/pass_mesh.cpp
    vulkan/draw/pass_decal.cpp
    vulkan/draw/pass_particles.cpp
    vulkan/draw/pass_billboard.cpp
    vulkan/draw/pass_gaussian_splat.cpp
    vulkan/ground/scene_cover_plan.cpp
    vulkan/ground/scene_mesh_drawer_grass.cpp
    vulkan/ground/scene_mesh_drawer_ground.cpp
    vulkan/ground/scene_mesh_drawer_land.cpp
    vulkan/ground/scene_land_process.cpp
    vulkan/ground/scene_land_regions.cpp
    vulkan/ground/scene_mesh_drawer_land_sun.cpp
    vulkan/foliage/scene_mesh_drawer_plants.cpp
    vulkan/foliage/scene_plants_gpu_plan.cpp
    vulkan/lighting/scene_lighting.cpp
    vulkan/lighting/scene_environment.cpp
    vulkan/lighting/scene_atmosphere.cpp
    vulkan/lighting/pass_atmosphere.cpp
    vulkan/lighting/pass_environment.cpp
    vulkan/lighting/pass_reflection_probe.cpp
    vulkan/shadow/pass_shadow.cpp
    vulkan/shadow/scene_caster_drawer.cpp
    vulkan/lighting/scene_probe_grid.cpp
    vulkan/shadow/pass_shadow_copy.cpp
    vulkan/shadow/pass_shadow_mark.cpp
    vulkan/shadow/scene_shadow_pages.cpp
    vulkan/post/pass_ssao.cpp
    vulkan/post/pass_ssr.cpp
    vulkan/post/pass_subpixel.cpp
    vulkan/post/pass_dof.cpp
    vulkan/post/scene_exposure.cpp
    vulkan/post/pass_postfx.cpp
    vulkan/sky/scene_clouds.cpp
    vulkan/sky/pass_clouds.cpp
    ${VK_GENERATED_HEADERS}
)

target_include_directories(bro_scene PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_scene PRIVATE ${VK_GEN_DIR})
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here.
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh FastNoise brogameagent bro_dom bro_render bro_layout bro_tile Vulkan::Vulkan)
# (The plants' canopy as GLSL, world/plant_canopy.cpp.)
target_link_libraries(bro_scene PRIVATE brokit_noise_glsl)
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

    # SurfaceLayers' tile bookkeeping (selection, jobs, slots, page table),
    # CPU only.
    add_executable(bro_surface_layers_test tests/test_surface_layers.cpp)
    target_link_libraries(bro_surface_layers_test PRIVATE bro_scene bro_render bro_platform bro_util)
    target_include_directories(bro_surface_layers_test PRIVATE ${bro_SOURCE_DIR}/src)

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

    # The probe grid's layout (layers, corners, the columns a move brings
    # in, volumes and refresh slices), CPU only: its source alone.
    add_executable(bro_probe_grid_test tests/test_probe_grid.cpp world/probe_grid.cpp)
    target_include_directories(bro_probe_grid_test PRIVATE ${bro_SOURCE_DIR}/src)

    # Placement (fields, rules, a cell's plants, the leaves kept about the
    # eye on the world's workers) and the cell store's runs, CPU only.
    add_executable(bro_plant_placement_test tests/test_plant_placement.cpp)
    target_link_libraries(bro_plant_placement_test PRIVATE bro_scene bro_render bro_platform bro_util)
    target_include_directories(bro_plant_placement_test PRIVATE ${bro_SOURCE_DIR}/src)

    # The atmosphere's tables against a CPU reference march (single
    # scattering brute force, multiple scattering by iterated orders).
    add_executable(bro_atmosphere_test tests/test_atmosphere.cpp tests/atmosphere_reference.cpp)
    target_link_libraries(bro_atmosphere_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_atmosphere_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})

    # The body and its place frame (anchors, frames, re-anchoring, walks
    # over a face's edge), CPU only: its source and bromath.
    add_executable(bro_place_test tests/test_place.cpp world/place.cpp)
    target_link_libraries(bro_place_test PRIVATE bromath)
    target_include_directories(bro_place_test PRIVATE ${bro_SOURCE_DIR}/src)

    # The land's processes' grid (roots, halos, sampling across a cube's
    # edges) and its regional grids' lattice and blending, CPU only: their
    # sources and the place's.
    add_executable(bro_land_grid_test tests/test_land_grid.cpp world/land_grid.cpp world/land_regions.cpp
                   world/place.cpp)
    target_link_libraries(bro_land_grid_test PRIVATE bromath)
    target_include_directories(bro_land_grid_test PRIVATE ${bro_SOURCE_DIR}/src)

    # FastNoise2 trees as GLSL (brokit's noise_glsl) against FastNoise2.
    add_executable(bro_noise_glsl_test tests/test_noise_glsl.cpp)
    target_link_libraries(bro_noise_glsl_test PRIVATE brokit_noise_glsl bro_render bro_util Vulkan::Vulkan)
    target_include_directories(bro_noise_glsl_test PRIVATE ${bro_SOURCE_DIR}/src)

    # The probe grid's world (the ground's height, the canopy, the sun
    # through it) against the scene it was made from.
    add_executable(bro_probe_grid_gpu_test tests/test_probe_grid_gpu.cpp)
    target_link_libraries(bro_probe_grid_gpu_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_probe_grid_gpu_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})

    # Live GPU SDF sphere-tracing, cone-traced soft shadows, ambient occlusion,
    # and SPIR-V 1.5 compute shader formal verification.
    add_executable(bro_sdf_gpu_tracer_test tests/test_sdf_gpu_tracer.cpp sdf_gpu_tracer.cpp sdf_gpu_vulkan.cpp)
    target_link_libraries(bro_sdf_gpu_tracer_test PRIVATE bromesh bromath brass bro_glsl bronze::cli)
    target_include_directories(bro_sdf_gpu_tracer_test PRIVATE ${bro_SOURCE_DIR}/src)
endif()


