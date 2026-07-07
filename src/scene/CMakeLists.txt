add_library(bro_scene STATIC
    scene_node.cpp
    scene_graph.cpp
    scene_renderer.cpp
    shape_node.cpp
    sprite_node.cpp
    physics_node.cpp
    mesh_node.cpp
    instanced_mesh_node.cpp
    gaussian_splat_node.cpp
    html_node.cpp
    light_node.cpp
    particle_node.cpp
    terrain_manager.cpp
    tile_world.cpp
    agent_binding.cpp
    ai_world_ticker.cpp
)

target_include_directories(bro_scene PUBLIC ${CMAKE_SOURCE_DIR}/src)
# bro_scene is only built when BRO_WITH_3D is on, and 3D forces PHYSICS + GAMEAI
# on (see the _bro_require chain in the top-level CMakeLists), so bro_physics +
# brogameagent are always present here.
target_link_libraries(bro_scene PUBLIC bro_canvas bro_physics bro_util broimage::broimage bromesh glad FastNoise brogameagent bro_dom bro_render bro_layout bro_tile)
target_compile_features(bro_scene PUBLIC cxx_std_20)
bro_apply_feature_defs(bro_scene)

if(MSVC)
    target_compile_options(bro_scene PRIVATE /MP /permissive- /utf-8)
endif()
