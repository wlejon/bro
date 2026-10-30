# bro_tilecache: the CPU half of the tile cache (bro.tiles): quadtree
# selection by projected cell size, slots, eviction, production order and the
# draw set. No GPU: bronze_host owns the slot pools and runs world producers.

add_library(bro_tilecache STATIC tile_tree.cpp)
target_include_directories(bro_tilecache PUBLIC ${bro_SOURCE_DIR}/src)
target_compile_features(bro_tilecache PUBLIC cxx_std_20)

if(BRO_BUILD_TESTS)
    add_executable(bro_tilecache_test tests/test_tile_tree.cpp)
    target_link_libraries(bro_tilecache_test PRIVATE bro_tilecache)
endif()
