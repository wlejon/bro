# bro_tile — pure tile-grid core (no rendering, no scene, no JS).
#
# Dependency-free C++20 so it stays trivially promotable to a standalone
# sibling library later. The scene-side tile renderer (a future TileWorld
# node) will link this; for now only the unit-test exe consumes it.

add_library(bro_tile STATIC
    coord.cpp
    grid.cpp
    region.cpp
    autotile.cpp
    pathfind.cpp
    serialize.cpp
)
target_include_directories(bro_tile PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(bro_tile PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(bro_tile PRIVATE /MP /permissive- /utf-8)
endif()

# Self-contained C++ unit tests. Build target: bro_tile_test.
# Run: ./build/<config>/bro_tile_test  (exit 0 = all pass).
add_executable(bro_tile_test
    tests/main.cpp
    tests/test_coord.cpp
    tests/test_grid.cpp
    tests/test_region.cpp
    tests/test_autotile.cpp
    tests/test_pathfind.cpp
    tests/test_serialize.cpp
)
target_link_libraries(bro_tile_test PRIVATE bro_tile)
target_compile_features(bro_tile_test PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(bro_tile_test PRIVATE /MP /permissive- /utf-8)
endif()
