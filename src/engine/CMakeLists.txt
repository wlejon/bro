add_library(bro_engine STATIC
    app_reload.cpp
    engine.cpp
    scene_audio_sync.cpp
    engine_compositor.cpp
    engine_init.cpp
    engine_lifecycle.cpp
    engine_threads.cpp
    engine_frame.cpp
    engine_frame_render.cpp
    css_transitions.cpp
    web_animations.cpp
    input_common.cpp
    input_editable.cpp
    input_keyboard.cpp
    input_mouse.cpp
    input_mouse_move.cpp
    input_text.cpp
    input_wheel_drag.cpp
    drag_drop.cpp
    action_input.cpp
    touch_input.cpp
    gamepad.cpp
    headless_api.cpp
    headless_driver.cpp
    key_mapping.cpp
    overflow.cpp
    scrollbar.cpp
    app_loader.cpp
    config_loader.cpp
    launcher.cpp
    replaced_elements.cpp
    replaced_elements_input.cpp
    system_panels.cpp
    iframe.cpp
    sub_document.cpp
    window_host.cpp
    window_host_input.cpp
    settings.cpp
    overlay.cpp
    dropdown_overlay.cpp
    color_picker_overlay.cpp
    menu_bar.cpp
    inspector_highlight.cpp
)
# navmesh_subsystem.cpp needs brogameagent; compile only when BRO_WITH_GAMEAI.
if(BRO_WITH_GAMEAI)
    target_sources(bro_engine PRIVATE navmesh_subsystem.cpp)
endif()
# gizmo.cpp / gizmo_routing.cpp are 3D-only (scene::MeshNode + bromesh); compile
# them only when BRO_WITH_3D. Their engine call sites are #if-guarded too.
if(BRO_WITH_3D)
    target_sources(bro_engine PRIVATE gizmo.cpp gizmo_mesh.cpp gizmo_routing.cpp)
endif()
target_include_directories(bro_engine PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_dom bro_layout bro_canvas bro_webgl bro_audio_inference bro_steam broaudio bro_util glad)
# <img> intrinsic sizing probes WebP headers too, so the engine needs the same
# decoder the two image paths use (render/webp_image.h).
if(BRO_WITH_WEBP)
    target_link_libraries(bro_engine PRIVATE bro_webp)
endif()
if(BRO_WITH_NET)
    target_link_libraries(bro_engine PUBLIC bro_net)
endif()
if(BRO_WITH_PHYSICS)
    target_link_libraries(bro_engine PUBLIC bro_physics)
endif()
if(BRO_WITH_3D)
    target_link_libraries(bro_engine PUBLIC bro_scene)
endif()
if(BRO_WITH_GAMEAI)
    target_link_libraries(bro_engine PUBLIC brogameagent)
endif()

# Stamp BRO_WITH_* compile definitions so engine_init.cpp can #if on features
# whose install() takes an engine-owned service pointer (net/audio/physics).
bro_apply_feature_defs(bro_engine)
