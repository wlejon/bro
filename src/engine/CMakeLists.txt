add_library(bro_engine STATIC
    engine.cpp
    input_handling.cpp
    headless_api.cpp
    key_mapping.cpp
    overflow.cpp
    hit_testing.cpp
    scrollbar.cpp
    app_loader.cpp
    system_overlay.cpp
)
target_include_directories(bro_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_js bro_dom bro_layout bro_canvas bro_webgl bro_audio bro_util glad)
