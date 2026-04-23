add_library(bro_video STATIC
    media_clock.cpp
)
target_include_directories(bro_video PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_video PUBLIC bro_util)
