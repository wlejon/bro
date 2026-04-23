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

## ── Library ──────────────────────────────────────────────────────────────────

add_library(bro_js STATIC
    runtime.cpp timers.cpp window_bindings.cpp
    dom_bindings.cpp node_bindings.cpp event_bindings.cpp style_bindings.cpp
    element_bindings.cpp shadowroot_bindings.cpp document_bindings.cpp
    range_bindings.cpp selection_bindings.cpp
    custom_elements.cpp event_dispatch.cpp mutation_observer.cpp
    canvas_bindings.cpp audio_bindings.cpp storage_bindings.cpp dialog_bindings.cpp
    webgl2_bindings.cpp
    webgl2_bindings_state.cpp
    webgl2_bindings_buffers.cpp
    webgl2_bindings_shaders.cpp
    webgl2_bindings_textures.cpp
    webgl2_bindings_framebuffers.cpp
    webgl2_bindings_queries.cpp
    image_bindings.cpp
    physics_bindings.cpp
    scene_bindings.cpp
    mesh_bindings.cpp
    rigging_bindings.cpp
    terrain_bindings.cpp
    settings_bindings.cpp
    net_bindings.cpp
    server_bindings.cpp
    crosshair_bindings.cpp
    gizmo_bindings.cpp
    menu_bindings.cpp
    ai_bindings.cpp
    ai_binding_integration.cpp
    headless_bindings.cpp
    message_serializer.cpp
    worker.cpp
    ${GENERATED_HEADERS}
)
target_include_directories(bro_js PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_js PRIVATE ${GEN_DIR})
target_link_libraries(bro_js PUBLIC qjs qjsbind brokit bro_dom bro_util bro_canvas bro_physics bro_scene bro_net broaudio bro_webgl stb_image SDL3::SDL3 bromesh brogameagent)
# QuickJS headers
target_include_directories(bro_js PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)
