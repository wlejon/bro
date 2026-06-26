// test_autotile.cpp — neighbour bitmask -> variant index.

#include "tile/tests/check.h"
#include "tile/tests/tests.h"

#include "tile/autotile.h"
#include "tile/grid.h"

#include <array>

namespace bro::tile::test {

void run_autotile_tests() {
    using namespace bro::tile;

    section("autotile.edgeMask.square");
    {
        TileGrid g(3, 3, Topology::Square, { "ground" });
        FamilyFn fam = familyTile(0, 1);
        Cell c{ 1, 1 };

        // No neighbours set -> 0.
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 0);

        g.setTile(0, Cell{ 2, 1 }, 1); // E -> bit0
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 1);
        g.setTile(0, Cell{ 2, 1 }, 0);

        g.setTile(0, Cell{ 1, 0 }, 1); // N -> bit1
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 2);
        g.setTile(0, Cell{ 1, 0 }, 0);

        g.setTile(0, Cell{ 0, 1 }, 1); // W -> bit2
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 4);
        g.setTile(0, Cell{ 0, 1 }, 0);

        g.setTile(0, Cell{ 1, 2 }, 1); // S -> bit3
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 8);
        g.setTile(0, Cell{ 1, 2 }, 0);

        // E + S together.
        g.setTile(0, Cell{ 2, 1 }, 1);
        g.setTile(0, Cell{ 1, 2 }, 1);
        CHECK_EQ(static_cast<int>(edgeMask(g, c, fam)), 1 | 8);
    }

    section("autotile.blobMask.square");
    {
        TileGrid g(3, 3, Topology::Square, { "ground" });
        FamilyFn fam = familyTile(0, 1);
        Cell c{ 1, 1 };

        g.setTile(0, Cell{ 2, 0 }, 1); // NE -> bit1
        CHECK_EQ(static_cast<int>(blobMask(g, c, fam)), 2);

        g.setTile(0, Cell{ 2, 1 }, 1); // E -> bit0
        g.setTile(0, Cell{ 1, 0 }, 1); // N -> bit2
        // E|NE|N = bit0|bit1|bit2 = 7
        CHECK_EQ(static_cast<int>(blobMask(g, c, fam)), 7);

        // SW corner (-1,+1) -> (0,2) -> bit5
        TileGrid g2(3, 3, Topology::Square, { "ground" });
        g2.setTile(0, Cell{ 0, 2 }, 1);
        CHECK_EQ(static_cast<int>(blobMask(g2, c, fam)), 1 << 5);
    }

    section("autotile.blob47.invariants");
    {
        std::array<char, 47> hit{};
        for (auto& h : hit) h = 0;
        int distinct = 0;
        int maxIdx = -1, minIdx = 999;
        for (int m = 0; m < 256; ++m) {
            int idx = blob47(static_cast<uint8_t>(m));
            CHECK(idx >= 0 && idx <= 46);
            if (idx < minIdx) minIdx = idx;
            if (idx > maxIdx) maxIdx = idx;
            if (idx >= 0 && idx < 47 && !hit[idx]) { hit[idx] = 1; ++distinct; }
            // Determinism: same input, same output.
            CHECK_EQ(blob47(static_cast<uint8_t>(m)), idx);
        }
        CHECK_EQ(distinct, 47); // exactly 47 normalized forms
        CHECK_EQ(minIdx, 0);
        CHECK_EQ(maxIdx, 46);

        // A corner bit with a missing adjacent edge collapses to the
        // corner-cleared mask. NE=bit1 needs E(bit0)+N(bit2).
        CHECK_EQ(blob47(1u << 1), blob47(0));       // NE alone == empty
        CHECK_EQ(blob47((1u << 1) | (1u << 0)),     // NE + only E (N missing)
                 blob47(1u << 0));                  //   == E alone
        // With both adjacent edges, the corner is a distinct form.
        CHECK(blob47((1u << 0) | (1u << 1) | (1u << 2)) !=
              blob47((1u << 0) | (1u << 2)));
    }

    section("autotile.wangCorners");
    {
        TileGrid g(3, 3, Topology::Square, { "ground" });
        FamilyFn fam = familyTile(0, 1);
        Cell c{ 1, 1 };

        g.setTile(0, Cell{ 2, 0 }, 1); // NE (+1,-1) -> bit0
        CHECK_EQ(static_cast<int>(wangCorners(g, c, fam)), 1);
        g.setTile(0, Cell{ 2, 0 }, 0);

        g.setTile(0, Cell{ 2, 2 }, 1); // SE (+1,+1) -> bit1
        CHECK_EQ(static_cast<int>(wangCorners(g, c, fam)), 2);
        g.setTile(0, Cell{ 2, 2 }, 0);

        g.setTile(0, Cell{ 0, 2 }, 1); // SW (-1,+1) -> bit2
        CHECK_EQ(static_cast<int>(wangCorners(g, c, fam)), 4);
        g.setTile(0, Cell{ 0, 2 }, 0);

        g.setTile(0, Cell{ 0, 0 }, 1); // NW (-1,-1) -> bit3
        CHECK_EQ(static_cast<int>(wangCorners(g, c, fam)), 8);

        // All four corners.
        g.setTile(0, Cell{ 2, 0 }, 1);
        g.setTile(0, Cell{ 2, 2 }, 1);
        g.setTile(0, Cell{ 0, 2 }, 1);
        CHECK_EQ(static_cast<int>(wangCorners(g, c, fam)), 15);
    }

    section("autotile.familyTile.oob");
    {
        TileGrid g(3, 3, Topology::Square, { "ground" });
        g.setTile(0, Cell{ 0, 0 }, 1);
        FamilyFn fam = familyTile(0, 1);
        CHECK(fam(g, Cell{ 0, 0 }) == true);
        CHECK(fam(g, Cell{ -1, 0 }) == false); // OOB not a member
        CHECK(fam(g, Cell{ 3, 3 }) == false);

        FamilyFn ne = familyNonEmpty(0);
        CHECK(ne(g, Cell{ 0, 0 }) == true);
        CHECK(ne(g, Cell{ 1, 1 }) == false); // empty
        CHECK(ne(g, Cell{ -1, -1 }) == false);
    }
}

} // namespace bro::tile::test
