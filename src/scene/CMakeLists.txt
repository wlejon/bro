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
# (custom mesh shaders, the clipmap terrain). SHADER_FILE is its path under
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
embed_vk_shader(common/scene_mesh_push.glsl     kVkSceneMeshPushSrc)
embed_vk_shader(common/scene_shadow_push.glsl   kVkSceneShadowPushSrc)
embed_vk_shader(common/scene_shadow_pages.glsl  kVkSceneShadowPagesSrc)
embed_vk_shader(shadow/shadow_pages_layout.h    kVkShadowPagesLayoutSrc)
embed_vk_shader(shadow/scene_shadow_cutout.glsl kVkSceneShadowCutoutSrc)
embed_vk_shader(common/scene_vegetation.glsl    kVkSceneVegetationSrc)
embed_vk_shader(common/scene_procedural.glsl    kVkSceneProceduralSrc)
embed_vk_shader(draw/scene_bark.glsl            kVkSceneBarkSrc)
embed_vk_shader(ground/scene_grass.glsl         kVkSceneGrassSrc)
embed_vk_shader(ground/scene_grass_surface.glsl kVkSceneGrassSurfaceSrc)
embed_vk_shader(ground/scene_ground.glsl        kVkSceneGroundSrc)
embed_vk_shader(ground/scene_ground_layers.glsl kVkSceneGroundLayersSrc)
embed_vk_shader(ground/scene_herb_mix.glsl      kVkSceneHerbMixSrc)
embed_vk_shader(ground/scene_herb_surface.glsl  kVkSceneHerbSurfaceSrc)
embed_vk_shader(terrain/clipmap_common.glsl       kClipmapCommonSrc)
embed_vk_shader(terrain/clipmap_detail.glsl       kClipmapDetailSrc)
embed_vk_shader(terrain/clipmap_material.glsl     kClipmapMaterialSrc)
embed_vk_shader(terrain/clipmap_cubic.glsl        kClipmapCubicSrc)
embed_vk_shader(terrain/clipmap_cubic_height.glsl kClipmapCubicHeightSrc)
embed_vk_shader(terrain/clipmap.vert.glsl         kClipmapVertSrc)
embed_vk_shader(terrain/clipmap.frag.glsl         kClipmapFragSrc)

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
    nodes/clipmap_terrain.cpp
    nodes/clipmap_terrain_elevation.cpp
    nodes/tile_world.cpp
    nodes/tile_world_mesh.cpp
    nodes/tile_world_objects.cpp
    world/world_cells.cpp
    world/surface_layers.cpp
    world/world_service.cpp
    world/shadow_pages.cpp
    world/shadow_pages_plan.cpp
    world/probe_grid.cpp
    vulkan/frame/scene_vk_device.cpp
    vulkan/frame/scene_vk_allocator.cpp
    vulkan/frame/scene_vk_pipeline.cpp
    vulkan/frame/scene_vk_descriptors.cpp
    vulkan/frame/scene_vk_shader_compiler.cpp
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

    # The atmosphere's tables against a CPU reference march (single
    # scattering brute force, multiple scattering by iterated orders).
    add_executable(bro_atmosphere_test tests/test_atmosphere.cpp tests/atmosphere_reference.cpp)
    target_link_libraries(bro_atmosphere_test PRIVATE bro_scene bro_engine bro_dom bro_engine
                          bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_atmosphere_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})
endif()


