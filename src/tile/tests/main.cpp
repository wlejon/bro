// main.cpp — tile-core unit test runner. Exits 0 on all-pass, 1 on any failure.

#include "tile/tests/check.h"
#include "tile/tests/tests.h"

#include <cstdio>

int main() {
    using namespace bro::tile::test;

    std::printf("bro_tile unit tests\n");
    run_coord_tests();
    run_grid_tests();
    run_region_tests();
    run_autotile_tests();
    run_pathfind_tests();
    run_serialize_tests();

    std::printf("---\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
