add_library(bro_physics STATIC
    physics_world.cpp
)

target_include_directories(bro_physics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bro_physics PUBLIC Jolt::Jolt bro_util)
target_compile_features(bro_physics PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(bro_physics PRIVATE /MP /permissive- /utf-8)
endif()
