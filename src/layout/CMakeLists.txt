add_library(bro_layout STATIC draw_traversal.cpp el_input.cpp el_textarea.cpp el_select.cpp el_svg.cpp el_video.cpp selection_geometry.cpp svg_geometry.cpp)
target_include_directories(bro_layout PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_layout PUBLIC htmlayout bro_render bro_svg bro_video bro_util broimage::broimage broaudio bro_dom)
