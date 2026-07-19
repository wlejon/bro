## ── Embed JS polyfills as C++ headers at build time ──────────────────────────
set(EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(embed_js JS_FILE VAR_NAME)
    set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/js/${JS_FILE}")
    set(OUTPUT "${GEN_DIR}/${JS_FILE}.h")
    add_custom_command(
        OUTPUT  "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${INPUT}"
                -DOUTPUT="${OUTPUT}"
                -DVAR_NAME="${VAR_NAME}"
                -P "${EMBED_SCRIPT}"
        DEPENDS "${INPUT}" "${EMBED_SCRIPT}"
        COMMENT "Embedding js/${JS_FILE} -> ${JS_FILE}.h"
    )
    list(APPEND GENERATED_HEADERS "${OUTPUT}")
    set(GENERATED_HEADERS "${GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

embed_js(dom_polyfills.js      js_dom_polyfills)
embed_js(observer_polyfills.js js_observer_polyfills)
embed_js(window_polyfill.js    js_window_polyfill)
embed_js(dataset_proxy.js      js_dataset_proxy)
embed_js(image_gpu.js          js_image_gpu)
embed_js(net_sync.js           js_net_sync)

## ── Library ──────────────────────────────────────────────────────────────────

add_library(bro_js STATIC
    asset_path.cpp
    runtime.cpp timers.cpp window_bindings.cpp window_host_bindings.cpp gamepad_bindings.cpp
    dom_bindings.cpp node_bindings.cpp event_bindings.cpp style_bindings.cpp
    element_bindings.cpp shadowroot_bindings.cpp document_bindings.cpp
    range_bindings.cpp selection_bindings.cpp
    custom_elements.cpp event_dispatch.cpp mutation_observer.cpp
    web_animation_bindings.cpp
    matchmedia_bindings.cpp
    canvas_bindings.cpp audio_bindings.cpp audio_scene_sync.cpp storage_bindings.cpp dialog_bindings.cpp
    webgl2_bindings.cpp
    webgl2_bindings_state.cpp
    webgl2_bindings_buffers.cpp
    webgl2_bindings_shaders.cpp
    webgl2_bindings_textures.cpp
    webgl2_bindings_framebuffers.cpp
    webgl2_bindings_queries.cpp
    webgl2_bindings_objects.cpp
    image_bindings.cpp
    imagebitmap_bindings.cpp
    video_bindings.cpp
    physics_bindings.cpp
    scene_bindings.cpp
    scene_bindings_mesh.cpp
    scene_bindings_anim.cpp
    scene_bindings_clip.cpp
    scene_bindings_fx.cpp
    scene_bindings_view.cpp
    mesh_bindings.cpp
    flora_bindings.cpp
    math_bindings.cpp
    rigging_bindings.cpp
    terrain_bindings.cpp
    tile_bindings.cpp
    settings_bindings.cpp
    net_bindings.cpp
    steam_bindings.cpp
    server_bindings.cpp
    gizmo_bindings.cpp
    menu_bindings.cpp
    time_bindings.cpp
    text_bindings.cpp
    ai_bindings.cpp
    ai_binding_integration.cpp
    ai_nn_bindings.cpp
    gpu_bindings.cpp
    tensor_bindings.cpp
    tensor_bindings_activations.cpp
    tensor_bindings_attention.cpp
    tensor_bindings_conv.cpp
    tensor_bindings_diffusion.cpp
    tensor_bindings_int8.cpp
    tensor_bindings_safetensors.cpp
    tensor_bindings_audio.cpp
    async_job.cpp
    diffusion_bindings.cpp
    lm_bindings.cpp
    stt_bindings.cpp
    tts_bindings.cpp
    diar_bindings.cpp
    rave_bindings.cpp
    vision_bindings.cpp
    triposplat_bindings.cpp
    motion_bindings.cpp
    wake_bindings.cpp
    listen_host.cpp
    listen_bindings.cpp
    kws_bindings.cpp
    sense_bindings.cpp
    gesture_bindings.cpp
    mic_bindings.cpp
    ai_learn_bindings.cpp
    ai_belief_bindings.cpp
    ai_extras_bindings.cpp
    ai_generic_mcts_bindings.cpp
    ai_grid_bindings.cpp
    ai_parallel_bindings.cpp
    headless_bindings.cpp
    message_serializer.cpp
    worker.cpp
    feature_stubs.cpp
    ${GENERATED_HEADERS}
)
target_include_directories(bro_js PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_js PRIVATE ${GEN_DIR})
# brolm is linked explicitly (not just transitively via brodiffusion):
# diffusion_bindings.cpp uses brolm::clip for diffusion text encoding, and
# lm_bindings.cpp exposes brolm::qwen as bro.lm.
target_link_libraries(bro_js PUBLIC qjs qjsbind brokit bro_dom bro_util bro_canvas bro_audio_inference bro_steam broaudio bro_webgl SDL3::SDL3 broimage::broimage)

# WebP: `new Image()` decodes here, and the renderer decodes in
# render/image_cache.cpp. Both go through bro_webp so the two cannot disagree
# about which formats exist — see render/webp_image.h.
if(BRO_WITH_WEBP)
    target_link_libraries(bro_js PRIVATE bro_webp)
endif()
# bro_scene + bromesh back the 3D scene graph / Mesh bindings (3D-only);
# brogameagent backs the bro.ai.game bindings (GAMEAI-only). Their *_bindings.cpp
# bodies are wrapped in the matching #if and their off-state stubs live in
# feature_stubs.cpp; the siblings are add_subdirectory'd conditionally too.
if(BRO_WITH_3D)
    target_link_libraries(bro_js PUBLIC bro_scene bromesh)
endif()
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro_js PUBLIC brogameagent)
endif()

# Tier-1 renderer/service sibling links, gated per feature. The matching
# *_bindings.cpp bodies are wrapped in `#if BRO_WITH_X` and their off-state
# installXBindings stubs live in feature_stubs.cpp; the siblings themselves are
# add_subdirectory'd conditionally in third_party/CMakeLists.txt.
if(BRO_WITH_FLORA)
    target_link_libraries(bro_js PUBLIC broflora)
endif()
if(BRO_WITH_NET)
    target_link_libraries(bro_js PUBLIC bro_net)
endif()
if(BRO_WITH_VIDEO)
    target_link_libraries(bro_js PUBLIC bro_video)
endif()
if(BRO_WITH_PHYSICS)
    target_link_libraries(bro_js PUBLIC bro_physics)
endif()

# AI-tower sibling links, gated per feature. The matching *_bindings.cpp are
# wrapped in `#if BRO_WITH_X` and their stubs live in feature_stubs.cpp; the
# siblings themselves are add_subdirectory'd conditionally in
# third_party/CMakeLists.txt. brotensor is pulled transitively (via broimage
# when BRO_WITH_TENSOR, or via these siblings) and needs no explicit link here.
if(BRO_WITH_LM)
    target_link_libraries(bro_js PUBLIC brolm)
endif()
if(BRO_WITH_DIFFUSION)
    target_link_libraries(bro_js PUBLIC brodiffusion)
endif()
if(BRO_WITH_SOUNDML)
    target_link_libraries(bro_js PUBLIC brosoundml::brosoundml)
endif()
if(BRO_WITH_VISION)
    target_link_libraries(bro_js PUBLIC brovisionml::brovisionml)
endif()

# Stamp BRO_WITH_* as compile definitions for the #if guards in the bindings
# (feature gating is added cluster-by-cluster; see docs/build-options.md).
bro_apply_feature_defs(bro_js)
# QuickJS headers
target_include_directories(bro_js PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)
