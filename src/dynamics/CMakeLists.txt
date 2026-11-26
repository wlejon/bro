add_library(bro_dynamics STATIC
    world.cpp
    structure.cpp
)

target_include_directories(bro_dynamics PUBLIC ${bro_SOURCE_DIR}/src)
# Box3D stays behind dynamics/world.h: nothing outside this directory includes its headers.
target_link_libraries(bro_dynamics PRIVATE box3d)
target_compile_features(bro_dynamics PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(bro_dynamics PRIVATE /MP /permissive- /utf-8)
endif()
