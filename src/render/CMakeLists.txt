add_library(bro_render STATIC renderer.cpp skia_backend.cpp)
target_include_directories(bro_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_render PUBLIC bro_platform skia bro_util)
