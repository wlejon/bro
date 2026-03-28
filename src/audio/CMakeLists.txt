add_library(bro_audio STATIC audio_engine.cpp)
target_include_directories(bro_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(TARGET SDL3::SDL3)
    target_link_libraries(bro_audio PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bro_audio PUBLIC SDL3::SDL3-static)
endif()

target_link_libraries(bro_audio PUBLIC bro_util)
