## ── Embed GLSL shaders as C++ headers at build time ──────────────────────────
## Each shaders/<file>.glsl becomes
## generated/<file>.h holding `static const char <VAR>[]` (a null-terminated byte
## array — dodges MSVC's ~16KB single-literal limit that mesh.frag exceeds).
set(EMBED_SCRIPT "${bro_SOURCE_DIR}/cmake/embed_js.cmake")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(embed_shader SHADER_FILE VAR_NAME)
    set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_FILE}")
    set(OUTPUT "${GEN_DIR}/${SHADER_FILE}.h")
    add_custom_command(
        OUTPUT  "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${INPUT}"
                -DOUTPUT="${OUTPUT}"
                -DVAR_NAME="${VAR_NAME}"
                -P "${EMBED_SCRIPT}"
        DEPENDS "${INPUT}" "${EMBED_SCRIPT}"
        COMMENT "Embedding shaders/${SHADER_FILE} -> ${SHADER_FILE}.h"
    )
    list(APPEND GENERATED_HEADERS "${OUTPUT}")
    set(GENERATED_HEADERS "${GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

embed_shader(mesh.vert             kMeshVertSrc)
embed_shader(mesh.frag             kMeshFragSrc)
embed_shader(mesh_instanced.vert   kMeshInstancedVertSrc)
embed_shader(foliage_scatter.vert  kFoliageScatterVertSrc)
embed_shader(branch_tube.vert      kBranchTubeVertSrc)
embed_shader(tonemap.vert          kTonemapVertSrc)
embed_shader(tonemap.frag          kTonemapFragSrc)
embed_shader(post.vert             kPostVertSrc)
embed_shader(blur.frag             kBlurFragSrc)
embed_shader(tilt_composite.frag   kTiltCompositeFragSrc)
embed_shader(bloom_bright.frag     kBloomBrightFragSrc)
embed_shader(ssao.frag             kSSAOFragSrc)
embed_shader(ssr.frag              kSSRFragSrc)
embed_shader(dof_composite.frag    kDoFCompositeFragSrc)
embed_shader(fxaa.frag             kFXAAFragSrc)
embed_shader(env_convert.vert      kEnvConvertVertSrc)
embed_shader(env_convert.frag      kEnvConvertFragSrc)
embed_shader(irradiance.frag       kIrradianceFragSrc)
embed_shader(prefilter.frag        kPrefilterFragSrc)
embed_shader(brdf_lut.frag         kBRDFLUTFragSrc)
embed_shader(skybox.vert           kSkyboxVertSrc)
embed_shader(skybox.frag           kSkyboxFragSrc)
embed_shader(starfield.frag        kStarfieldFragSrc)
embed_shader(shadow.vert           kShadowVertSrc)
embed_shader(shadow.frag           kShadowFragSrc)
embed_shader(shadow_instanced.vert kShadowInstancedVertSrc)
embed_shader(billboard.vert        kBillboardVertSrc)
embed_shader(billboard.frag        kBillboardFragSrc)
embed_shader(particles3d.vert      kParticles3DVertSrc)
embed_shader(particles3d.frag      kParticles3DFragSrc)
embed_shader(decal.vert            kDecalVertSrc)
embed_shader(decal.frag            kDecalFragSrc)
# Clipmap-terrain user chunks — spliced into mesh.vert / mesh.frag at the
# //__USER_CHUNK__ markers via MeshNode::setCustomShader, not compiled alone.
embed_shader(clipmap_common.glsl   kClipmapCommonSrc)
embed_shader(atmosphere.glsl       kAtmosphereSrc)
embed_shader(sky_atmosphere.frag   kSkyAtmosphereFragSrc)
embed_shader(clipmap_detail.glsl   kClipmapDetailSrc)
embed_shader(clipmap_material.glsl kClipmapMaterialSrc)
# Opt-in only (ClipmapConfig::cubicSurface). Spliced between the shared chunks
# and the material chunk when the flag is on, and not appended at all when it
# is off — which is what keeps the default source byte-identical.
embed_shader(clipmap_cubic.glsl    kClipmapCubicSrc)
# Opt-in only (ClipmapConfig::cubicHeight). The same idiom one field over, and
# spliced into BOTH stages rather than the fragment alone: it reconstructs the
# HEIGHT the vertex stage displaces with and the fragment stage shades from, and
# those two have to be the same surface.
embed_shader(clipmap_cubic_height.glsl kClipmapCubicHeightSrc)
embed_shader(clipmap.vert.glsl     kClipmapVertSrc)
embed_shader(clipmap.frag.glsl     kClipmapFragSrc)

## ── Vulkan SPIR-V shader compilation ──────────────────────────────────────────
find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(VK_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_vk")
file(MAKE_DIRECTORY "${VK_GEN_DIR}")

function(compile_vk_shader_header SHADER_FILE STAGE)
    set(INPUT "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/${SHADER_FILE}")
    set(OUTPUT "${VK_GEN_DIR}/${SHADER_FILE}.spv.h")
    add_custom_command(
        OUTPUT "${OUTPUT}"
        COMMAND ${GLSLC_EXECUTABLE} -fshader-stage=${STAGE} -mfmt=c "${INPUT}" -o "${OUTPUT}"
        DEPENDS "${INPUT}"
        COMMENT "Compiling SPIR-V vulkan/shaders/${SHADER_FILE} -> ${SHADER_FILE}.spv.h"
    )
    list(APPEND VK_GENERATED_HEADERS "${OUTPUT}")
    set(VK_GENERATED_HEADERS "${VK_GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

compile_vk_shader_header(mesh.vert vertex)
compile_vk_shader_header(mesh_instanced.vert vertex)
compile_vk_shader_header(mesh_skinned.vert vertex)
compile_vk_shader_header(mesh.frag fragment)
compile_vk_shader_header(shadow.vert vertex)
compile_vk_shader_header(shadow_instanced.vert vertex)
compile_vk_shader_header(shadow_skinned.vert vertex)
compile_vk_shader_header(shadow.frag fragment)
compile_vk_shader_header(skybox.vert vertex)
compile_vk_shader_header(environment.frag fragment)
compile_vk_shader_header(postfx.vert vertex)
compile_vk_shader_header(tonemap.frag fragment)
compile_vk_shader_header(bloom.frag fragment)
compile_vk_shader_header(fxaa.frag fragment)
compile_vk_shader_header(billboard.vert vertex)
compile_vk_shader_header(billboard.frag fragment)
compile_vk_shader_header(particles.vert vertex)
compile_vk_shader_header(particles.frag fragment)
compile_vk_shader_header(decal.vert vertex)
compile_vk_shader_header(decal.frag fragment)
compile_vk_shader_header(blur.frag fragment)
compile_vk_shader_header(color_lut.frag fragment)
compile_vk_shader_header(ssao.frag fragment)
compile_vk_shader_header(ssr.frag fragment)
compile_vk_shader_header(dof.frag fragment)
compile_vk_shader_header(apply_ao.frag fragment)
compile_vk_shader_header(gaussian_splat.vert vertex)
compile_vk_shader_header(gaussian_splat.frag fragment)

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

## ── Library ──────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    impostor_layer.cpp
    depth_policy.cpp
    scene_renderer.cpp
    scene_renderer_mesh.cpp
    scene_renderer_instanced.cpp
    scene_renderer_particles.cpp
    scene_renderer_decals.cpp
    scene_renderer_lighting.cpp
    scene_renderer_shadow.cpp
    scene_renderer_postfx.cpp
    scene_renderer_ssr.cpp
    scene_renderer_environment.cpp
    scene_renderer_overlays.cpp
    scene_renderer_probes.cpp
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
    tile_world.cpp
    tile_world_mesh.cpp
    tile_world_objects.cpp
    agent_binding.cpp
    ai_world_ticker.cpp
    vulkan/scene_vk_device.cpp
    vulkan/scene_vk_allocator.cpp
    vulkan/scene_vk_pipeline.cpp
    vulkan/scene_vk_descriptors.cpp
    vulkan/scene_vk_target.cpp
    vulkan/scene_vk_shader_compiler.cpp
    vulkan/scene_vk_custom_shader.cpp
    vulkan/pass_mesh.cpp
    vulkan/pass_shadow.cpp
    vulkan/pass_environment.cpp
    vulkan/pass_postfx.cpp
    vulkan/pass_billboard.cpp
    vulkan/pass_particles.cpp
    vulkan/pass_decal.cpp
    vulkan/pass_reflection_probe.cpp
    vulkan/pass_terrain.cpp
    vulkan/pass_ssao.cpp
    vulkan/pass_ssr.cpp
    vulkan/pass_dof.cpp
    vulkan/pass_color_lut.cpp
    vulkan/pass_gaussian_splat.cpp
    vulkan/scene_vk_bridge.cpp
    vulkan/scene_vk_bridge_resources.cpp
    vulkan/scene_vk_bridge_passes.cpp
    ${GENERATED_HEADERS}
    ${VK_GENERATED_HEADERS}
)

target_include_directories(bro_scene PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_scene PRIVATE ${GEN_DIR} ${VK_GEN_DIR})
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here.
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh FastNoise brogameagent bro_dom bro_render bro_layout bro_tile Vulkan::Vulkan)
if(BRO_HAS_SHADERC)
    target_link_libraries(bro_scene PUBLIC ${BRO_SHADERC_TARGET})
endif()
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
    target_link_libraries(bro_vulkan_scene_passes_test PRIVATE bro_scene bro_render bro_platform bro_util skia Vulkan::Vulkan)
    target_include_directories(bro_vulkan_scene_passes_test PRIVATE ${bro_SOURCE_DIR}/src ${VK_GEN_DIR})
endif()


