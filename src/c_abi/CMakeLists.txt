# bro/src/c_abi/CMakeLists.txt — Direct C-ABI Layer
add_library(bro_c_abi STATIC
    bro_engine_c_abi.cpp
    bro_math_c_abi.cpp
    bro_time_c_abi.cpp
    bro_paths_c_abi.cpp
    bro_noise_c_abi.cpp
    bro_dialogs_c_abi.cpp
    bro_window_c_abi.cpp
    bro_settings_c_abi.cpp
    bro_menu_c_abi.cpp
    bro_mic_c_abi.cpp
    bro_gamepad_c_abi.cpp
    bro_media_c_abi.cpp
    bro_listen_c_abi.cpp
    bro_audio_c_abi.cpp
    bro_steam_c_abi.cpp
    bro_server_c_abi.cpp
    bro_net_c_abi.cpp
    bro_worker_c_abi.cpp
    bro_abort_c_abi.cpp
    bro_file_c_abi.cpp
    bro_intl_c_abi.cpp
    bro_text_c_abi.cpp
    bro_domparser_c_abi.cpp
    bro_events_c_abi.cpp
    bro_canvas_c_abi.cpp
    bro_webgl2_c_abi.cpp
    bro_image_c_abi.cpp
    bro_imagebitmap_c_abi.cpp
    bro_gpu_c_abi.cpp
    bro_scene_c_abi.cpp
    bro_mesh_c_abi.cpp
    bro_lighting_c_abi.cpp
    bro_gizmo_c_abi.cpp
    bro_animation_c_abi.cpp
    bro_physics_c_abi.cpp
    bro_terrain_c_abi.cpp
    bro_clipmap_c_abi.cpp
    bro_tile_world_c_abi.cpp
    bro_flora_c_abi.cpp
    bro_rigging_c_abi.cpp
    bro_motion_c_abi.cpp
    bro_triposplat_c_abi.cpp
    bro_web_animations_c_abi.cpp
    bro_ai_c_abi.cpp
    bro_tensor_c_abi.cpp
    bro_vision_c_abi.cpp
    bro_diffusion_c_abi.cpp
    bro_lm_c_abi.cpp
    bro_stt_c_abi.cpp
    bro_tts_c_abi.cpp
    bro_kws_c_abi.cpp
    bro_diar_c_abi.cpp
    bro_rave_c_abi.cpp
)


target_include_directories(bro_c_abi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${bro_SOURCE_DIR}/include
    ${bro_SOURCE_DIR}/src
    ${bro_SOURCE_DIR}/third_party/bromath/include
    ${bro_SOURCE_DIR}/third_party/brokit/third_party/FastNoise2/include
)

target_link_libraries(bro_c_abi PUBLIC bromath FastNoise)
target_compile_features(bro_c_abi PUBLIC cxx_std_20)
