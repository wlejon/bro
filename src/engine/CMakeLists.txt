add_library(bro_engine STATIC engine.cpp app_loader.cpp)
target_include_directories(bro_engine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_engine PUBLIC bro_platform bro_render bro_js bro_dom bro_layout bro_canvas bro_audio bro_util)
