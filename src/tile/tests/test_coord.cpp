// test_coord.cpp — unit coverage for coord.h topology + coordinate math.

#include "tile/coord.h"
#include "tile/tests/check.h"

#include <algorithm>
#include <vector>

namespace bro::tile::test {

namespace {

bool has(const std::vector<Cell>& v, Cell c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

void run_coord_tests() {
    // ---------------------------------------------------------------------
    section("neighbors");
    // ---------------------------------------------------------------------
    {
        // Square Edge around (5,5): 0:E 1:N 2:W 3:S.
        Neighbors n = neighbors(Topology::Square, Cell{ 5, 5 }, Conn::Edge);
        CHECK_EQ(n.count, 4);
        CHECK(n[0] == (Cell{ 6, 5 }));   // E
        CHECK(n[1] == (Cell{ 5, 4 }));   // N
        CHECK(n[2] == (Cell{ 4, 5 }));   // W
        CHECK(n[3] == (Cell{ 5, 6 }));   // S
    }
    {
        // Square Vertex around (5,5): 0:E 1:NE 2:N 3:NW 4:W 5:SW 6:S 7:SE.
        Neighbors n = neighbors(Topology::Square, Cell{ 5, 5 }, Conn::Vertex);
        CHECK_EQ(n.count, 8);
        CHECK(n[0] == (Cell{ 6, 5 }));
        CHECK(n[1] == (Cell{ 6, 4 }));
        CHECK(n[2] == (Cell{ 5, 4 }));
        CHECK(n[3] == (Cell{ 4, 4 }));
        CHECK(n[4] == (Cell{ 4, 5 }));
        CHECK(n[5] == (Cell{ 4, 6 }));
        CHECK(n[6] == (Cell{ 5, 6 }));
        CHECK(n[7] == (Cell{ 6, 6 }));
    }
    {
        // Hex around (2,2) (even row). Expected 6 neighbours, conn ignored.
        Neighbors n = neighbors(Topology::Hex, Cell{ 2, 2 });
        CHECK_EQ(n.count, 6);
        std::vector<Cell> got(n.begin(), n.end());
        CHECK(has(got, Cell{ 3, 2 }));
        CHECK(has(got, Cell{ 2, 1 }));
        CHECK(has(got, Cell{ 1, 1 }));
        CHECK(has(got, Cell{ 1, 2 }));
        CHECK(has(got, Cell{ 1, 3 }));
        CHECK(has(got, Cell{ 2, 3 }));
        // Conn argument must not change hex neighbours.
        Neighbors nv = neighbors(Topology::Hex, Cell{ 2, 2 }, Conn::Vertex);
        CHECK_EQ(nv.count, 6);
    }

    // ---------------------------------------------------------------------
    section("distance");
    // ---------------------------------------------------------------------
    {
        CHECK_EQ(distance(Topology::Square, Cell{ 0, 0 }, Cell{ 3, 4 }, Conn::Edge), 7);
        CHECK_EQ(distance(Topology::Square, Cell{ 0, 0 }, Cell{ 3, 4 }, Conn::Vertex), 4);
        CHECK_EQ(distance(Topology::Square, Cell{ -2, -3 }, Cell{ 1, 1 }, Conn::Edge), 7);
        CHECK_EQ(distance(Topology::Square, Cell{ -2, -3 }, Cell{ 1, 1 }, Conn::Vertex), 4);

        // Hex distances (conn ignored).
        CHECK_EQ(distance(Topology::Hex, Cell{ 0, 0 }, Cell{ 0, 0 }), 0);
        CHECK_EQ(distance(Topology::Hex, Cell{ 2, 2 }, Cell{ 3, 2 }), 1);  // neighbour
        CHECK_EQ(distance(Topology::Hex, Cell{ 0, 0 }, Cell{ 0, 2 }), 2);  // even rows
        CHECK_EQ(distance(Topology::Hex, Cell{ 0, 1 }, Cell{ 0, 3 }), 2);  // odd rows
        CHECK_EQ(distance(Topology::Hex, Cell{ -2, -1 }, Cell{ 1, 2 }), 4);// negatives
        // hexDistance directly on axial.
        CHECK_EQ(hexDistance(toHex(Cell{ 0, 0 }), toHex(Cell{ 0, 0 })), 0);
        CHECK_EQ(hexDistance(Hex{ 0, 0 }, Hex{ 3, 0 }), 3);
    }

    // ---------------------------------------------------------------------
    section("hex roundtrip");
    // ---------------------------------------------------------------------
    {
        // toHex/fromHex must round-trip exactly, including negatives.
        for (int y = -5; y <= 20; ++y)
            for (int x = -5; x <= 20; ++x) {
                Cell c{ x, y };
                CHECK(fromHex(toHex(c)) == c);
            }
    }

    // ---------------------------------------------------------------------
    section("ring cardinality");
    // ---------------------------------------------------------------------
    {
        Cell c{ 4, 4 };
        // r < 0 -> empty; r == 0 -> {center}.
        CHECK_EQ(static_cast<int>(ring(Topology::Square, c, -1, Conn::Edge).size()), 0);
        CHECK_EQ(static_cast<int>(ring(Topology::Square, c, 0, Conn::Edge).size()), 1);
        CHECK(ring(Topology::Square, c, 0, Conn::Edge)[0] == c);
        CHECK_EQ(static_cast<int>(ring(Topology::Hex, c, 0).size()), 1);

        for (int r = 1; r <= 5; ++r) {
            CHECK_EQ(static_cast<int>(ring(Topology::Square, c, r, Conn::Edge).size()), 4 * r);
            CHECK_EQ(static_cast<int>(ring(Topology::Square, c, r, Conn::Vertex).size()), 8 * r);
            CHECK_EQ(static_cast<int>(ring(Topology::Hex, c, r).size()), 6 * r);
        }

        // Membership spot checks (center 0,0).
        auto re = ring(Topology::Square, Cell{ 0, 0 }, 2, Conn::Edge);
        CHECK(has(re, Cell{ 2, 0 }));
        CHECK(has(re, Cell{ 1, 1 }));     // Manhattan == 2
        CHECK(!has(re, Cell{ 1, 0 }));    // Manhattan == 1
        auto rv = ring(Topology::Square, Cell{ 0, 0 }, 2, Conn::Vertex);
        CHECK(has(rv, Cell{ 2, 1 }));     // Chebyshev == 2
        CHECK(!has(rv, Cell{ 1, 1 }));    // Chebyshev == 1
        // Hex ring(1) equals the 6 neighbours.
        auto rh = ring(Topology::Hex, Cell{ 2, 2 }, 1);
        CHECK(has(rh, Cell{ 3, 2 }));
        CHECK(has(rh, Cell{ 1, 1 }));
        // Every hex ring cell is exactly `radius` away.
        for (int r = 1; r <= 4; ++r)
            for (const Cell& cell : ring(Topology::Hex, Cell{ 5, 5 }, r))
                CHECK_EQ(distance(Topology::Hex, Cell{ 5, 5 }, cell), r);
    }

    // ---------------------------------------------------------------------
    section("range cardinality");
    // ---------------------------------------------------------------------
    {
        Cell c{ 7, 7 };
        CHECK_EQ(static_cast<int>(range(Topology::Square, c, -1, Conn::Edge).size()), 0);
        CHECK_EQ(static_cast<int>(range(Topology::Square, c, 0, Conn::Edge).size()), 1);
        CHECK_EQ(static_cast<int>(range(Topology::Hex, c, 0).size()), 1);

        for (int r = 0; r <= 5; ++r) {
            CHECK_EQ(static_cast<int>(range(Topology::Square, c, r, Conn::Vertex).size()),
                     (2 * r + 1) * (2 * r + 1));
            CHECK_EQ(static_cast<int>(range(Topology::Square, c, r, Conn::Edge).size()),
                     2 * r * r + 2 * r + 1);
            CHECK_EQ(static_cast<int>(range(Topology::Hex, c, r).size()),
                     3 * r * r + 3 * r + 1);
        }
        // Center is included and every cell is within radius.
        auto rg = range(Topology::Square, Cell{ 0, 0 }, 3, Conn::Edge);
        CHECK(has(rg, Cell{ 0, 0 }));
        for (const Cell& cell : rg)
            CHECK(distance(Topology::Square, Cell{ 0, 0 }, cell, Conn::Edge) <= 3);
    }

    // ---------------------------------------------------------------------
    section("line");
    // ---------------------------------------------------------------------
    {
        // Single cell when a == b.
        auto s = line(Topology::Square, Cell{ 3, 3 }, Cell{ 3, 3 });
        CHECK_EQ(static_cast<int>(s.size()), 1);
        CHECK(s[0] == (Cell{ 3, 3 }));

        // Square line: endpoints correct, 4-connected (each step 1 axis by 1).
        auto verifySquare = [&](Cell a, Cell b) {
            auto ln = line(Topology::Square, a, b);
            CHECK(ln.front() == a);
            CHECK(ln.back() == b);
            for (size_t i = 1; i < ln.size(); ++i) {
                int dx = std::abs(ln[i].x - ln[i - 1].x);
                int dy = std::abs(ln[i].y - ln[i - 1].y);
                CHECK_EQ(dx + dy, 1);   // exactly one orthogonal step
            }
        };
        verifySquare(Cell{ 0, 0 }, Cell{ 5, 2 });
        verifySquare(Cell{ 0, 0 }, Cell{ 2, 5 });
        verifySquare(Cell{ 5, 3 }, Cell{ 0, 0 });   // reversed / negative dir
        verifySquare(Cell{ -3, -2 }, Cell{ 4, 1 });
        verifySquare(Cell{ 2, 2 }, Cell{ 2, 7 });   // vertical
        verifySquare(Cell{ 2, 2 }, Cell{ 7, 2 });   // horizontal
        verifySquare(Cell{ 0, 0 }, Cell{ 4, 4 });   // pure diagonal staircase

        // Hex line: single cell at zero distance; length == distance + 1.
        auto h0 = line(Topology::Hex, Cell{ 1, 1 }, Cell{ 1, 1 });
        CHECK_EQ(static_cast<int>(h0.size()), 1);
        CHECK(h0[0] == (Cell{ 1, 1 }));
        auto verifyHex = [&](Cell a, Cell b) {
            auto ln = line(Topology::Hex, a, b);
            CHECK(ln.front() == a);
            CHECK(ln.back() == b);
            int d = distance(Topology::Hex, a, b);
            CHECK_EQ(static_cast<int>(ln.size()), d + 1);
            // Consecutive cells are hex-adjacent.
            for (size_t i = 1; i < ln.size(); ++i)
                CHECK_EQ(distance(Topology::Hex, ln[i - 1], ln[i]), 1);
        };
        verifyHex(Cell{ 0, 0 }, Cell{ 4, 3 });
        verifyHex(Cell{ 1, 1 }, Cell{ -3, 5 });
        verifyHex(Cell{ 5, 5 }, Cell{ 0, 0 });
    }
}

} // namespace bro::tile::test
