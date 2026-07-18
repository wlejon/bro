## ── Embed GLSL shaders as C++ headers at build time ──────────────────────────
## Mirrors the pattern in src/js/CMakeLists.txt. Each shaders/<file>.glsl becomes
## generated/<file>.h holding `static const char <VAR>[]` (a null-terminated byte
## array — dodges MSVC's ~16KB single-literal limit that mesh.frag exceeds).
set(EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake")
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
embed_shader(tonemap.vert          kTonemapVertSrc)
embed_shader(tonemap.frag          kTonemapFragSrc)
embed_shader(post.vert             kPostVertSrc)
embed_shader(blur.frag             kBlurFragSrc)
embed_shader(tilt_composite.frag   kTiltCompositeFragSrc)
embed_shader(bloom_bright.frag     kBloomBrightFragSrc)
embed_shader(ssao.frag             kSSAOFragSrc)
embed_shader(dof_composite.frag    kDoFCompositeFragSrc)
embed_shader(fxaa.frag             kFXAAFragSrc)
embed_shader(env_convert.vert      kEnvConvertVertSrc)
embed_shader(env_convert.frag      kEnvConvertFragSrc)
embed_shader(irradiance.frag       kIrradianceFragSrc)
embed_shader(prefilter.frag        kPrefilterFragSrc)
embed_shader(brdf_lut.frag         kBRDFLUTFragSrc)
embed_shader(skybox.vert           kSkyboxVertSrc)
embed_shader(skybox.frag           kSkyboxFragSrc)
embed_shader(shadow.vert           kShadowVertSrc)
embed_shader(shadow.frag           kShadowFragSrc)
embed_shader(shadow_instanced.vert kShadowInstancedVertSrc)
embed_shader(billboard.vert        kBillboardVertSrc)
embed_shader(billboard.frag        kBillboardFragSrc)
embed_shader(particles3d.vert      kParticles3DVertSrc)
embed_shader(particles3d.frag      kParticles3DFragSrc)
embed_shader(decal.vert            kDecalVertSrc)
embed_shader(decal.frag            kDecalFragSrc)

## ── Library ──────────────────────────────────────────────────────────────────
add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    scene_renderer.cpp
    scene_renderer_mesh.cpp
    scene_renderer_instanced.cpp
    scene_renderer_particles.cpp
    scene_renderer_decals.cpp
    scene_renderer_lighting.cpp
    scene_renderer_shadow.cpp
    scene_renderer_postfx.cpp
    scene_renderer_environment.cpp
    scene_renderer_overlays.cpp
    shape_node.cpp
    sprite_node.cpp
    physics_node.cpp
    mesh_node.cpp
    skinned_mesh_node.cpp
    animation_player.cpp
    tween.cpp
    instanced_mesh_node.cpp
    gaussian_splat_node.cpp
    html_node.cpp
    light_node.cpp
    particle_node.cpp
    particles3d_node.cpp
    decal_node.cpp
    terrain_manager.cpp
    tile_world.cpp
    agent_binding.cpp
    ai_world_ticker.cpp
    ${GENERATED_HEADERS}
)

target_include_directories(bro_scene PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_scene PRIVATE ${GEN_DIR})
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here.
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh glad FastNoise brogameagent bro_dom bro_render bro_layout bro_tile)
target_compile_features(bro_scene PUBLIC cxx_std_20)
bro_apply_feature_defs(bro_scene)

if(MSVC)
    target_compile_options(bro_scene PRIVATE /MP /permissive- /utf-8)
endif()
