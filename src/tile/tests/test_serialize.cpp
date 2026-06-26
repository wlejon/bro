#include "tile/tests/check.h"
#include "tile/tests/tests.h"

#include "tile/serialize.h"
#include "tile/grid.h"
#include "tile/coord.h"

#include <optional>
#include <string>
#include <vector>

namespace bro::tile::test {

void run_serialize_tests() {
    // -- Round-trip: multi-layer hex grid -------------------------------------
    section("serialize/roundtrip");
    {
        TileGrid g(5, 4, Topology::Hex, {"ground", "object"});

        // Paint tiles on each layer.
        g.setTile(0, Cell{0, 0}, 1);
        g.setTile(0, Cell{4, 3}, 42);
        g.setTile(0, Cell{2, 1}, 7);
        g.setTile(1, Cell{0, 0}, 100);
        g.setTile(1, Cell{3, 2}, 65535);
        g.setTile(1, Cell{4, 0}, 9);

        // Elevation: mix of negative and positive.
        g.setElevation(Cell{0, 0}, -5);
        g.setElevation(Cell{1, 1}, 32000);
        g.setElevation(Cell{4, 3}, -32000);
        g.setElevation(Cell{2, 2}, 3);

        // Flags: assorted bitmasks.
        g.setFlags(Cell{0, 0}, 0x1u);
        g.setFlags(Cell{1, 0}, 0xDEADBEEFu);
        g.setFlags(Cell{4, 3}, 0xFFFFFFFFu);
        g.setFlags(Cell{2, 2}, 0x80000000u);

        std::vector<uint8_t> bytes = serialize(g);
        CHECK(!bytes.empty());

        std::optional<TileGrid> rt = deserialize(bytes);
        CHECK(rt.has_value());
        if (rt) {
            const TileGrid& d = *rt;
            CHECK(d.topology() == Topology::Hex);
            CHECK_EQ(d.width(), 5);
            CHECK_EQ(d.height(), 4);
            CHECK_EQ(d.layerCount(), 2);
            CHECK(d.layerName(0) == "ground");
            CHECK(d.layerName(1) == "object");

            // Every cell on every plane must match.
            bool cellsMatch = true;
            for (int y = 0; y < g.height(); ++y) {
                for (int x = 0; x < g.width(); ++x) {
                    Cell c{x, y};
                    for (int layer = 0; layer < g.layerCount(); ++layer) {
                        if (d.tile(layer, c) != g.tile(layer, c))
                            cellsMatch = false;
                    }
                    if (d.elevation(c) != g.elevation(c))
                        cellsMatch = false;
                    if (d.flags(c) != g.flags(c))
                        cellsMatch = false;
                }
            }
            CHECK(cellsMatch);

            // Byte-stable: re-serializing the deserialized grid is identical.
            std::vector<uint8_t> bytes2 = serialize(d);
            CHECK(bytes == bytes2);
        }
    }

    // -- Header / identity ----------------------------------------------------
    section("serialize/header");
    {
        TileGrid g(3, 2, Topology::Square, {"base"});
        std::vector<uint8_t> bytes = serialize(g);
        CHECK(bytes.size() >= 8);
        CHECK_EQ(bytes[0], static_cast<uint8_t>('B'));
        CHECK_EQ(bytes[1], static_cast<uint8_t>('T'));
        CHECK_EQ(bytes[2], static_cast<uint8_t>('I'));
        CHECK_EQ(bytes[3], static_cast<uint8_t>('L'));
        CHECK_EQ(bytes[4], 1);                                  // version
        CHECK_EQ(bytes[5], static_cast<uint8_t>(Topology::Square)); // topology
        CHECK_EQ(bytes[6], 1);                                  // layerCount
        CHECK_EQ(bytes[7], 0);                                  // reserved

        // Hex topology byte.
        TileGrid h(3, 2, Topology::Hex, {"base"});
        std::vector<uint8_t> hb = serialize(h);
        CHECK_EQ(hb[5], static_cast<uint8_t>(Topology::Hex));
    }

    // -- Robustness -----------------------------------------------------------
    section("serialize/robustness");
    {
        // Empty buffer.
        CHECK(!deserialize(std::vector<uint8_t>{}).has_value());

        TileGrid g(4, 3, Topology::Square, {"a", "b"});
        g.setTile(0, Cell{1, 1}, 5);
        g.setElevation(Cell{2, 2}, -9);
        g.setFlags(Cell{3, 0}, 0xABCDu);
        std::vector<uint8_t> good = serialize(g);
        CHECK(deserialize(good).has_value());

        // Bad magic.
        std::vector<uint8_t> badMagic = good;
        badMagic[0] = 'X';
        CHECK(!deserialize(badMagic).has_value());

        // Bad version.
        std::vector<uint8_t> badVer = good;
        badVer[4] = 2;
        CHECK(!deserialize(badVer).has_value());

        // Truncated (drop last byte).
        std::vector<uint8_t> truncated = good;
        truncated.pop_back();
        CHECK(!deserialize(truncated).has_value());

        // Trailing garbage (append an extra byte) -> exact-size check fails.
        std::vector<uint8_t> trailing = good;
        trailing.push_back(0xAA);
        CHECK(!deserialize(trailing).has_value());
    }

    // -- Minimal 1x1 single-layer ---------------------------------------------
    section("serialize/minimal");
    {
        TileGrid g(1, 1, Topology::Square, {"only"});
        g.setTile(0, Cell{0, 0}, 123);
        g.setElevation(Cell{0, 0}, -1);
        g.setFlags(Cell{0, 0}, 0x7u);

        std::optional<TileGrid> rt = deserialize(serialize(g));
        CHECK(rt.has_value());
        if (rt) {
            const TileGrid& d = *rt;
            CHECK_EQ(d.width(), 1);
            CHECK_EQ(d.height(), 1);
            CHECK_EQ(d.layerCount(), 1);
            CHECK(d.layerName(0) == "only");
            CHECK_EQ(d.tile(0, Cell{0, 0}), 123);
            CHECK_EQ(d.elevation(Cell{0, 0}), -1);
            CHECK_EQ(d.flags(Cell{0, 0}), 0x7u);
        }
    }
}

} // namespace bro::tile::test
