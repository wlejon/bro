// test_grid.cpp — unit coverage for grid.h TileGrid.

#include "tile/grid.h"
#include "tile/tests/check.h"

#include <string>
#include <vector>

namespace bro::tile::test {

void run_grid_tests() {
    // ---------------------------------------------------------------------
    section("construction");
    // ---------------------------------------------------------------------
    {
        TileGrid g(10, 8, Topology::Square, { "ground", "objects" });
        CHECK_EQ(g.width(), 10);
        CHECK_EQ(g.height(), 8);
        CHECK_EQ(g.cellCount(), 80);
        CHECK_EQ(g.layerCount(), 2);
        CHECK(g.topology() == Topology::Square);
        CHECK_EQ(g.layerIndex("ground"), 0);
        CHECK_EQ(g.layerIndex("objects"), 1);
        CHECK_EQ(g.layerIndex("missing"), -1);
        CHECK(g.layerName(0) == std::string("ground"));
        // All cells start empty.
        CHECK_EQ(g.tile(0, Cell{ 0, 0 }), 0);
        CHECK_EQ(g.elevation(Cell{ 5, 5 }), 0);
        CHECK_EQ(g.flags(Cell{ 5, 5 }), 0u);
    }

    // ---------------------------------------------------------------------
    section("tiles per layer");
    // ---------------------------------------------------------------------
    {
        TileGrid g(6, 6, Topology::Square, { "a", "b" });
        g.setTile(0, Cell{ 2, 3 }, 5);
        CHECK_EQ(g.tile(0, Cell{ 2, 3 }), 5);
        CHECK_EQ(g.tile(1, Cell{ 2, 3 }), 0);   // other layer untouched
        g.setTile(1, Cell{ 2, 3 }, 9);
        CHECK_EQ(g.tile(1, Cell{ 2, 3 }), 9);
        CHECK_EQ(g.tile(0, Cell{ 2, 3 }), 5);   // first layer preserved
    }

    // ---------------------------------------------------------------------
    section("oob policy");
    // ---------------------------------------------------------------------
    {
        TileGrid g(4, 4, Topology::Square, { "a" });
        g.setTile(0, Cell{ 1, 1 }, 7);
        // OOB reads return zero.
        CHECK_EQ(g.tile(0, Cell{ -1, 0 }), 0);
        CHECK_EQ(g.tile(0, Cell{ 4, 0 }), 0);
        CHECK_EQ(g.tile(0, Cell{ 0, 4 }), 0);
        CHECK_EQ(g.elevation(Cell{ -1, -1 }), 0);
        CHECK_EQ(g.flags(Cell{ 100, 100 }), 0u);
        CHECK(!g.hasFlag(Cell{ 100, 100 }, 0x1));
        CHECK(!g.inBounds(Cell{ 4, 0 }));
        CHECK(g.inBounds(Cell{ 3, 3 }));
        // OOB writes are no-ops and don't disturb in-bounds data.
        g.setTile(0, Cell{ 100, 100 }, 42);
        g.setElevation(Cell{ -5, -5 }, 99);
        g.setFlags(Cell{ -1, -1 }, 0xFFFF);
        CHECK_EQ(g.tile(0, Cell{ 1, 1 }), 7);
    }

    // ---------------------------------------------------------------------
    section("elevation");
    // ---------------------------------------------------------------------
    {
        TileGrid g(5, 5, Topology::Square, { "a" });
        g.setElevation(Cell{ 1, 1 }, -50);
        CHECK_EQ(g.elevation(Cell{ 1, 1 }), -50);
        g.setElevation(Cell{ 2, 2 }, 1200);
        CHECK_EQ(g.elevation(Cell{ 2, 2 }), 1200);
        CHECK_EQ(g.elevation(Cell{ 0, 0 }), 0);
    }

    // ---------------------------------------------------------------------
    section("flags");
    // ---------------------------------------------------------------------
    {
        TileGrid g(5, 5, Topology::Square, { "a" });
        g.setFlags(Cell{ 0, 0 }, 0b1010);
        CHECK_EQ(g.flags(Cell{ 0, 0 }), 0b1010u);
        g.setFlag(Cell{ 0, 0 }, 0b0100, true);     // -> 1110
        CHECK_EQ(g.flags(Cell{ 0, 0 }), 0b1110u);
        g.setFlag(Cell{ 0, 0 }, 0b1000, false);    // clear bit3 -> 0110
        CHECK_EQ(g.flags(Cell{ 0, 0 }), 0b0110u);
        // Multi-bit hasFlag: all requested bits must be present.
        g.setFlags(Cell{ 1, 1 }, 0b1100);
        CHECK(g.hasFlag(Cell{ 1, 1 }, 0b1100));
        CHECK(g.hasFlag(Cell{ 1, 1 }, 0b0100));
        CHECK(!g.hasFlag(Cell{ 1, 1 }, 0b0010));
        CHECK(!g.hasFlag(Cell{ 1, 1 }, 0b0110));   // 0100 present but 0010 missing
    }

    // ---------------------------------------------------------------------
    section("fill / fillRect");
    // ---------------------------------------------------------------------
    {
        TileGrid g(8, 8, Topology::Square, { "a", "b" });
        g.fill(0, 3);
        CHECK_EQ(g.tile(0, Cell{ 0, 0 }), 3);
        CHECK_EQ(g.tile(0, Cell{ 7, 7 }), 3);
        CHECK_EQ(g.tile(1, Cell{ 7, 7 }), 0);      // other layer untouched

        // Normal rectangle [2..4] x [2..5].
        g.fillRect(0, Cell{ 2, 2 }, Cell{ 4, 5 }, 7);
        CHECK_EQ(g.tile(0, Cell{ 2, 2 }), 7);
        CHECK_EQ(g.tile(0, Cell{ 4, 5 }), 7);
        CHECK_EQ(g.tile(0, Cell{ 3, 3 }), 7);
        CHECK_EQ(g.tile(0, Cell{ 1, 2 }), 3);      // just outside -> still fill value
        CHECK_EQ(g.tile(0, Cell{ 5, 2 }), 3);

        // Reversed corners describe the same rectangle.
        g.fillRect(1, Cell{ 4, 5 }, Cell{ 2, 2 }, 8);
        CHECK_EQ(g.tile(1, Cell{ 2, 2 }), 8);
        CHECK_EQ(g.tile(1, Cell{ 4, 5 }), 8);
        CHECK_EQ(g.tile(1, Cell{ 3, 4 }), 8);

        // Clamping: corners outside bounds are clamped to the edge.
        TileGrid h(4, 4, Topology::Square, { "a" });
        h.fillRect(0, Cell{ -2, -2 }, Cell{ 1, 1 }, 9);
        CHECK_EQ(h.tile(0, Cell{ 0, 0 }), 9);
        CHECK_EQ(h.tile(0, Cell{ 1, 1 }), 9);
        CHECK_EQ(h.tile(0, Cell{ 2, 2 }), 0);      // outside the rect
        h.fillRect(0, Cell{ 2, 2 }, Cell{ 99, 99 }, 4);
        CHECK_EQ(h.tile(0, Cell{ 3, 3 }), 4);      // clamped to the far corner
        CHECK_EQ(h.tile(0, Cell{ 2, 2 }), 4);
        // Rectangle entirely outside is a no-op.
        h.fillRect(0, Cell{ 50, 50 }, Cell{ 60, 60 }, 1);
        CHECK_EQ(h.tile(0, Cell{ 3, 3 }), 4);
    }

    // ---------------------------------------------------------------------
    section("index / cellOf");
    // ---------------------------------------------------------------------
    {
        TileGrid g(10, 8, Topology::Square, { "a" });
        CHECK_EQ(static_cast<long long>(g.index(Cell{ 3, 2 })), 23ll); // 2*10 + 3
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 10; ++x) {
                Cell c{ x, y };
                CHECK(g.cellOf(g.index(c)) == c);
            }
    }

    // ---------------------------------------------------------------------
    section("grow preserve no offset");
    // ---------------------------------------------------------------------
    {
        TileGrid g(3, 3, Topology::Hex, { "a", "b" });
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                g.setTile(0, Cell{ x, y }, static_cast<uint16_t>(x + y * 3 + 1));
                g.setElevation(Cell{ x, y }, static_cast<int16_t>(x - y));
                g.setFlags(Cell{ x, y }, static_cast<uint32_t>((x + 1) * 16 + y));
            }
        g.grow(5, 5, 0, 0);
        CHECK_EQ(g.width(), 5);
        CHECK_EQ(g.height(), 5);
        CHECK_EQ(g.layerCount(), 2);
        CHECK(g.topology() == Topology::Hex);          // topology preserved
        CHECK_EQ(g.layerIndex("b"), 1);                // layer names preserved
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                CHECK_EQ(g.tile(0, Cell{ x, y }), static_cast<uint16_t>(x + y * 3 + 1));
                CHECK_EQ(g.elevation(Cell{ x, y }), static_cast<int16_t>(x - y));
                CHECK_EQ(g.flags(Cell{ x, y }), static_cast<uint32_t>((x + 1) * 16 + y));
            }
        // Newly exposed cells are empty.
        CHECK_EQ(g.tile(0, Cell{ 4, 4 }), 0);
        CHECK_EQ(g.tile(0, Cell{ 0, 4 }), 0);
        CHECK_EQ(g.elevation(Cell{ 4, 0 }), 0);
        CHECK_EQ(g.flags(Cell{ 3, 3 }), 0u);
    }

    // ---------------------------------------------------------------------
    section("grow with offset");
    // ---------------------------------------------------------------------
    {
        TileGrid g(3, 3, Topology::Square, { "a" });
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                g.setTile(0, Cell{ x, y }, static_cast<uint16_t>(x + y * 3 + 1));
                g.setElevation(Cell{ x, y }, static_cast<int16_t>(10 - x - y));
            }
        g.grow(6, 6, 2, 1);   // old (x,y) -> (x+2, y+1)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                CHECK_EQ(g.tile(0, Cell{ x + 2, y + 1 }), static_cast<uint16_t>(x + y * 3 + 1));
                CHECK_EQ(g.elevation(Cell{ x + 2, y + 1 }), static_cast<int16_t>(10 - x - y));
            }
        // Region not covered by the shifted content is empty.
        CHECK_EQ(g.tile(0, Cell{ 0, 0 }), 0);
        CHECK_EQ(g.tile(0, Cell{ 1, 0 }), 0);
        CHECK_EQ(g.tile(0, Cell{ 5, 5 }), 0);
    }

    // ---------------------------------------------------------------------
    section("grow drops out-of-range");
    // ---------------------------------------------------------------------
    {
        TileGrid g(3, 3, Topology::Square, { "a" });
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                g.setTile(0, Cell{ x, y }, static_cast<uint16_t>(x + y * 3 + 1));
        g.grow(2, 2, 0, 0);   // x==2 and y==2 cells fall outside -> dropped
        CHECK_EQ(g.width(), 2);
        CHECK_EQ(g.height(), 2);
        CHECK_EQ(g.tile(0, Cell{ 0, 0 }), 1);
        CHECK_EQ(g.tile(0, Cell{ 1, 0 }), 2);
        CHECK_EQ(g.tile(0, Cell{ 0, 1 }), 4);
        CHECK_EQ(g.tile(0, Cell{ 1, 1 }), 5);
        // (2,*) and (*,2) are gone — those cells no longer exist.
        CHECK(!g.inBounds(Cell{ 2, 0 }));
        CHECK(!g.inBounds(Cell{ 0, 2 }));
    }
}

} // namespace bro::tile::test
