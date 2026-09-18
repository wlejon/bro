add_library(bro_physics STATIC
    physics_decomposition.cpp
    physics_world.cpp
)

target_include_directories(bro_physics PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_physics PUBLIC Jolt::Jolt bro_util)
target_compile_features(bro_physics PUBLIC cxx_std_20)

if(BRO_WITH_3D)
    target_link_libraries(bro_physics PUBLIC bromesh)
    target_compile_definitions(bro_physics PUBLIC BRO_WITH_3D=1)
endif()

if(MSVC)
    target_compile_options(bro_physics PRIVATE /MP /permissive- /utf-8)
endif()
