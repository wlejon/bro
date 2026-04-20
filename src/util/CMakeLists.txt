add_library(bro_util STATIC log.cpp string_utils.cpp time.cpp interrupt.cpp)
target_include_directories(bro_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_util PUBLIC qjs)
