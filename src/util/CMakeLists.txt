add_library(bro_util STATIC log.cpp string_utils.cpp)
target_include_directories(bro_util PUBLIC ${CMAKE_SOURCE_DIR}/src)
