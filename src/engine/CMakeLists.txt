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
    css_transitions.cpp
    input_handling.cpp
    headless_api.cpp
    key_mapping.cpp
    overflow.cpp
    scrollbar.cpp
    app_loader.cpp
    replaced_elements.cpp
    system_panels.cpp
    settings.cpp
    overlay.cpp
    dropdown_overlay.cpp
    color_picker_overlay.cpp
    gizmo.cpp
    gizmo_routing.cpp
    menu_bar.cpp
    ${GENERATED_HEADERS}
)
target_include_directories(bro_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(bro_engine PRIVATE ${GEN_DIR})
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_js bro_dom bro_layout bro_canvas bro_webgl bro_physics bro_scene bro_net broaudio bro_util glad)
