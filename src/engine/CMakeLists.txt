add_library(bro_engine STATIC
    app_reload.cpp
    source_preflight.cpp
    screen_capture.cpp
    engine.cpp
    scene_audio_sync.cpp
    engine_compositor.cpp
    engine_frame_composite.cpp
    engine_compile_frame.cpp
    engine_init.cpp
    engine_init_drm.cpp
    engine_run_drm.cpp
    engine_drm_input.cpp
    shell_input_rules.cpp
    engine_shell_compositor.cpp
    window_frames.cpp
    engine_lifecycle.cpp
    engine_threads.cpp
    engine_frame.cpp
    engine_frame_render.cpp
    css_transitions.cpp
    css_animations.cpp
    css_easing.cpp
    css_interpolation.cpp
    css_transform_interpolation.cpp
    element_activation.cpp
    web_animations.cpp
    web_animations_effect.cpp
    input_common.cpp
    input_editable.cpp
    input_keyboard.cpp
    input_mouse.cpp
    input_mouse_move.cpp
    input_text.cpp
    input_terminal.cpp
    input_terminal_mouse.cpp
    terminal_layers.cpp
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
    desktop_trust.cpp
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
    inspector_tree.cpp
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
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_dom bro_layout bro_canvas bro_webgl bro_audio_inference bro_steam broaudio bro_util)
# The source watcher behind the edit loop (app_reload.cpp) is brokit's
# FsWatcher, the same one behind fs.watch.
target_link_libraries(bro_engine PRIVATE brokit_api)
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
if(BRO_WITH_A11Y)
    target_link_libraries(bro_engine PUBLIC bro_a11y)
endif()
if(BRO_WITH_COMPOSITOR)
    target_link_libraries(bro_engine PUBLIC bro_compositor)
endif()

if(BRO_BUILD_TESTS)
    # The reload preflight alone: it needs the manifest reader and nothing
    # else of the engine.
    add_executable(bro_source_preflight_test
        tests/test_source_preflight.cpp source_preflight.cpp app_loader.cpp)
    target_include_directories(bro_source_preflight_test PRIVATE ${bro_SOURCE_DIR}/src)
    target_link_libraries(bro_source_preflight_test PRIVATE bro_util)
    target_compile_features(bro_source_preflight_test PRIVATE cxx_std_20)
endif()

# Stamp BRO_WITH_* compile definitions so engine_init.cpp can #if on features
# whose install() takes an engine-owned service pointer (net/audio/physics).
bro_apply_feature_defs(bro_engine)
