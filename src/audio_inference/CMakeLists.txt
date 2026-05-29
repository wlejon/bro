add_library(bro_audio_inference STATIC
    audio_inference.cpp
)

target_include_directories(bro_audio_inference PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_audio_inference PUBLIC bro_util)
target_compile_features(bro_audio_inference PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(bro_audio_inference PRIVATE /MP /permissive- /utf-8)
endif()
