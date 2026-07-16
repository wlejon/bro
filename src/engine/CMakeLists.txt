## ── Embed JS scripts as C++ headers at build time ────────────────────────────
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

embed_js(observer_check.js  js_observer_check)

## ── Library ──────────────────────────────────────────────────────────────────

add_library(bro_engine STATIC
    engine.cpp
    engine_compositor.cpp
    engine_init.cpp
    engine_lifecycle.cpp
    engine_threads.cpp
    engine_frame.cpp
    css_transitions.cpp
    input_handling.cpp
    gamepad.cpp
    headless_api.cpp
    key_mapping.cpp
    overflow.cpp
    scrollbar.cpp
    app_loader.cpp
    config_loader.cpp
    replaced_elements.cpp
    system_panels.cpp
    iframe.cpp
    settings.cpp
    overlay.cpp
    dropdown_overlay.cpp
    color_picker_overlay.cpp
    menu_bar.cpp
    inspector_highlight.cpp
    ${GENERATED_HEADERS}
)
# gizmo.cpp / gizmo_routing.cpp are 3D-only (scene::MeshNode + bromesh); compile
# them only when BRO_WITH_3D. Their engine call sites are #if-guarded too.
if(BRO_WITH_3D)
    target_sources(bro_engine PRIVATE gizmo.cpp gizmo_routing.cpp)
endif()
target_include_directories(bro_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_engine PRIVATE ${GEN_DIR})
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_js bro_dom bro_layout bro_canvas bro_webgl bro_audio_inference bro_steam broaudio bro_util glad)
if(BRO_WITH_NET)
    target_link_libraries(bro_engine PUBLIC bro_net)
endif()
if(BRO_WITH_PHYSICS)
    target_link_libraries(bro_engine PUBLIC bro_physics)
endif()
if(BRO_WITH_3D)
    target_link_libraries(bro_engine PUBLIC bro_scene)
endif()

# Stamp BRO_WITH_* compile definitions so engine_init.cpp can #if on features
# whose install() takes an engine-owned service pointer (net/audio/physics).
bro_apply_feature_defs(bro_engine)
