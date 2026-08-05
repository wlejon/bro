add_library(bro_core STATIC
    bro_api.cpp
)

target_include_directories(bro_core PUBLIC
    ${bro_SOURCE_DIR}/include
    ${bro_SOURCE_DIR}/src
)

target_link_libraries(bro_core PUBLIC
    bro_scene
    bro_render
    bro_webgl
    bro_audio_inference
    broaudio
    bro_platform
    bro_util
    bro_canvas
    bro_dom
    bro_layout
    bro_svg
    bro_tile
    bromath
    bromesh
    glad
)

if(BRO_WITH_PHYSICS)
    target_link_libraries(bro_core PUBLIC bro_physics)
endif()

if(BRO_WITH_WEBP)
    target_link_libraries(bro_core PRIVATE bro_webp)
endif()

if(COMMAND bro_apply_feature_defs)
    bro_apply_feature_defs(bro_core)
endif()

add_library(bro_api ALIAS bro_core)
