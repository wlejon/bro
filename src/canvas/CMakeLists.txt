add_library(bro_canvas STATIC canvas2d.cpp canvas_scene.cpp canvas_scene_raster.cpp canvas_scene_state.cpp)
target_include_directories(bro_canvas PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_canvas PUBLIC bro_render)
