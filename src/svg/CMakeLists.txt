add_library(bro_svg STATIC svg_parser.cpp svg_renderer.cpp)
target_include_directories(bro_svg PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_svg PUBLIC litehtml bro_render bro_util)
