#pragma once

// tests.h — registry of tile-core test entry points. Each test_*.cpp implements
// one of these; main.cpp invokes them in order.

namespace bro::tile::test {

void run_coord_tests();
void run_grid_tests();
void run_region_tests();
void run_autotile_tests();
void run_pathfind_tests();
void run_serialize_tests();

} // namespace bro::tile::test
