# bro_tilecache: the CPU half of the tile cache (bro.tiles): quadtree
# selection by projected cell size, slots, eviction, production order and the
# draw set. No GPU: bronze_host owns the slot pools and runs world producers.
# tiles.bk, the brokernel module that reads a cache from a shader or kernel,
# is embedded as text (tiles_module.h, bro.tiles.module).

set(_tiles_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_tiles_gen}")
add_custom_command(
    OUTPUT  "${_tiles_gen}/tiles.bk.src.h"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/tiles.bk"
            -DOUTPUT="${_tiles_gen}/tiles.bk.src.h"
            -DVAR_NAME=kBkTilesSrc
            -P "${bro_SOURCE_DIR}/cmake/embed_js.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tiles.bk" "${bro_SOURCE_DIR}/cmake/embed_js.cmake"
    COMMENT "Embedding tilecache/tiles.bk -> tiles.bk.src.h"
)

add_library(bro_tilecache STATIC tile_tree.cpp tiles_module.cpp "${_tiles_gen}/tiles.bk.src.h")
target_include_directories(bro_tilecache PUBLIC ${bro_SOURCE_DIR}/src)
target_include_directories(bro_tilecache PRIVATE "${_tiles_gen}")
target_compile_features(bro_tilecache PUBLIC cxx_std_20)

if(BRO_BUILD_TESTS)
    add_executable(bro_tilecache_test tests/test_tile_tree.cpp)
    target_link_libraries(bro_tilecache_test PRIVATE bro_tilecache)
endif()
