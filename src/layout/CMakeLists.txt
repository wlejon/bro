add_library(bro_layout STATIC container.cpp font_manager.cpp bro_element.cpp el_input.cpp el_textarea.cpp el_select.cpp el_svg.cpp)
target_include_directories(bro_layout PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_layout PUBLIC litehtml bro_render bro_svg bro_util)
