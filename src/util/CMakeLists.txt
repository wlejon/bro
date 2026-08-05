add_library(bro_util STATIC log.cpp string_utils.cpp time.cpp interrupt.cpp asset_mounts.cpp)
target_include_directories(bro_util PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_util PUBLIC bromath::bromath)
target_link_libraries(bro_util PRIVATE qjs)
