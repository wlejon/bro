// test_region.cpp — flood-fill + connected-component selection.

#include "tile/tests/check.h"
#include "tile/tests/tests.h"

#include "tile/grid.h"
#include "tile/region.h"

#include <vector>

namespace bro::tile::test {

void run_region_tests() {
    using namespace bro::tile;

    section("region.floodFill.diagonalBridge");
    {
        // (1,1) and (2,2) are diagonal-only neighbours: Edge cannot cross the
        // gap, Vertex can.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.setTile(0, Cell{ 1, 1 }, 1);
        g.setTile(0, Cell{ 2, 2 }, 1);
        MatchFn m = matchTile(0, 1);

        std::vector<Cell> edge = floodFill(g, Cell{ 1, 1 }, m, Conn::Edge);
        CHECK_EQ(static_cast<int>(edge.size()), 1);
        CHECK(edge.front() == (Cell{ 1, 1 })); // seed first

        std::vector<Cell> vtx = floodFill(g, Cell{ 1, 1 }, m, Conn::Vertex);
        CHECK_EQ(static_cast<int>(vtx.size()), 2);
        CHECK(vtx.front() == (Cell{ 1, 1 }));
        CHECK(vtx[1] == (Cell{ 2, 2 }));
    }

    section("region.floodFill.failAndOOB");
    {
        TileGrid g(4, 4, Topology::Square, { "ground" });
        g.setTile(0, Cell{ 0, 0 }, 1);
        MatchFn m = matchTile(0, 1);

        // Seed on a non-matching cell -> empty.
        CHECK_EQ(static_cast<int>(floodFill(g, Cell{ 2, 2 }, m).size()), 0);
        // OOB seed -> empty.
        CHECK_EQ(static_cast<int>(floodFill(g, Cell{ -1, -1 }, m).size()), 0);
        CHECK_EQ(static_cast<int>(floodFill(g, Cell{ 4, 0 }, m).size()), 0);
        // Valid seed.
        CHECK_EQ(static_cast<int>(floodFill(g, Cell{ 0, 0 }, m).size()), 1);
    }

    section("region.floodFill.blob");
    {
        // A solid 3x3 block of tile 1 in a 5x5 grid.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.fillRect(0, Cell{ 1, 1 }, Cell{ 3, 3 }, 1);
        MatchFn m = matchTile(0, 1);
        std::vector<Cell> blob = floodFill(g, Cell{ 2, 2 }, m, Conn::Edge);
        CHECK_EQ(static_cast<int>(blob.size()), 9);
        CHECK(blob.front() == (Cell{ 2, 2 }));
    }

    section("region.components.separateBlobs");
    {
        // Blob A (edge-connected L) and Blob B, not touching.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.setTile(0, Cell{ 0, 0 }, 1);
        g.setTile(0, Cell{ 1, 0 }, 1);
        g.setTile(0, Cell{ 0, 1 }, 1); // A = 3 cells
        g.setTile(0, Cell{ 3, 3 }, 1);
        g.setTile(0, Cell{ 4, 3 }, 1); // B = 2 cells
        MatchFn m = matchTile(0, 1);

        auto comps = components(g, m, Conn::Edge);
        CHECK_EQ(static_cast<int>(comps.size()), 2);
        // Scan order: A (top-left) first.
        CHECK(comps[0].front() == (Cell{ 0, 0 }));
        CHECK_EQ(static_cast<int>(comps[0].size()), 3);
        CHECK_EQ(static_cast<int>(comps[1].size()), 2);

        // Every matching cell covered exactly once.
        int total = 0;
        std::vector<char> seen(static_cast<size_t>(g.cellCount()), 0);
        for (auto& c : comps)
            for (Cell cell : c) {
                size_t i = g.index(cell);
                CHECK(seen[i] == 0);
                seen[i] = 1;
                ++total;
            }
        CHECK_EQ(total, 5);
    }

    section("region.components.vertexMergesDiagonal");
    {
        // Same two diagonal cells: one component under Vertex, two under Edge.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.setTile(0, Cell{ 1, 1 }, 1);
        g.setTile(0, Cell{ 2, 2 }, 1);
        MatchFn m = matchTile(0, 1);
        CHECK_EQ(static_cast<int>(components(g, m, Conn::Edge).size()), 2);
        CHECK_EQ(static_cast<int>(components(g, m, Conn::Vertex).size()), 1);
    }

    section("region.matchFlag");
    {
        TileGrid g(3, 3, Topology::Square, { "ground" });
        const uint32_t WALK = 0x2;
        g.setFlag(Cell{ 0, 0 }, WALK, true);
        g.setFlag(Cell{ 1, 0 }, WALK, true);
        auto comps = components(g, matchFlag(WALK), Conn::Edge);
        CHECK_EQ(static_cast<int>(comps.size()), 1);
        CHECK_EQ(static_cast<int>(comps[0].size()), 2);
    }
}

} // namespace bro::tile::test
