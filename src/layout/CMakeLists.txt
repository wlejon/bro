add_library(bro_layout STATIC container.cpp font_manager.cpp)
target_include_directories(bro_layout PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_layout PUBLIC litehtml bro_render bro_util)
