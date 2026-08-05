add_library(bro_core STATIC
    bro_api.cpp
)

target_include_directories(bro_core PUBLIC
    ${bro_SOURCE_DIR}/include
    ${bro_SOURCE_DIR}/src
)

target_link_libraries(bro_core PUBLIC
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
    glad
)

# bro_scene and bromesh are only added to the build when BRO_WITH_3D is on
# (src/CMakeLists.txt / the root CMakeLists). Naming them unconditionally here
# was not a configure error — CMake takes an unknown name for a raw library —
# but it made bro_core unlinkable in a 3D-off build, on top of bro_api.cpp not
# compiling there at all. The 3D half of the façade is guarded to match.
if(BRO_WITH_3D)
    target_link_libraries(bro_core PUBLIC bro_scene bromesh)
endif()

if(BRO_WITH_WEBP)
    target_link_libraries(bro_core PRIVATE bro_webp)
endif()

if(COMMAND bro_apply_feature_defs)
    bro_apply_feature_defs(bro_core)
endif()

add_library(bro_api ALIAS bro_core)
