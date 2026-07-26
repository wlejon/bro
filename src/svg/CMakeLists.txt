add_library(bro_svg STATIC svg_renderer.cpp)
target_include_directories(bro_svg PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_svg PUBLIC skia skia_svg bro_util)
