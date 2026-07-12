// test_pathfind.cpp — BFS distance fields + A*.

#include "tile/tests/check.h"
#include "tile/tests/tests.h"

#include "tile/grid.h"
#include "tile/pathfind.h"

#include <vector>

namespace bro::tile::test {

static bool alwaysPass(const TileGrid&, Cell) { return true; }

// Every consecutive pair in `path` is a neighbour under (topo, conn).
static bool contiguous(const TileGrid& g, const std::vector<Cell>& path, Conn conn) {
    for (size_t i = 1; i < path.size(); ++i)
        if (distance(g.topology(), path[i - 1], path[i], conn) != 1) return false;
    return true;
}

void run_pathfind_tests() {
    using namespace bro::tile;

    const uint32_t BLOCK = 0x1;
    PassFn pass = passUnlessFlag(BLOCK);

    section("pathfind.distanceField.singleSource");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        auto field = distanceField(g, { Cell{ 0, 0 } }, alwaysPass, Conn::Edge);
        CHECK_EQ(static_cast<int>(field.size()), 25);
        CHECK_EQ(field[g.index(Cell{ 0, 0 })], 0);
        CHECK_EQ(field[g.index(Cell{ 4, 4 })], 8); // Manhattan
        CHECK_EQ(field[g.index(Cell{ 2, 3 })], 5);
        CHECK_EQ(field[g.index(Cell{ 4, 0 })], 4);
    }

    section("pathfind.distanceField.multiSource");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        auto field = distanceField(g, { Cell{ 0, 0 }, Cell{ 4, 4 } }, alwaysPass, Conn::Edge);
        CHECK_EQ(field[g.index(Cell{ 2, 2 })], 4); // min(4,4)
        CHECK_EQ(field[g.index(Cell{ 4, 0 })], 4);
        CHECK_EQ(field[g.index(Cell{ 0, 4 })], 4);
        CHECK_EQ(field[g.index(Cell{ 4, 4 })], 0);
    }

    section("pathfind.distanceField.wallUnreachable");
    {
        // Impassable column x=2 splits the grid; right side unreachable.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        for (int y = 0; y < 5; ++y) g.setFlag(Cell{ 2, y }, BLOCK, true);
        auto field = distanceField(g, { Cell{ 0, 0 } }, pass, Conn::Edge);
        CHECK_EQ(field[g.index(Cell{ 0, 0 })], 0);
        CHECK_EQ(field[g.index(Cell{ 1, 4 })], 5); // reachable left side
        CHECK_EQ(field[g.index(Cell{ 2, 1 })], -1); // wall cell
        CHECK_EQ(field[g.index(Cell{ 4, 4 })], -1); // right side cut off
        CHECK_EQ(field[g.index(Cell{ 3, 0 })], -1);
    }

    section("pathfind.distanceField.wallWithGap");
    {
        // Wall x=2 for y=0..3, gap at (2,4).
        TileGrid g(5, 5, Topology::Square, { "ground" });
        for (int y = 0; y < 4; ++y) g.setFlag(Cell{ 2, y }, BLOCK, true);
        auto field = distanceField(g, { Cell{ 0, 0 } }, pass, Conn::Edge);
        CHECK_EQ(field[g.index(Cell{ 2, 4 })], 6); // through the gap: down 4 + over 2
        CHECK_EQ(field[g.index(Cell{ 4, 0 })], 12); // gap (2,4)=6, then 6 more up the right side
        CHECK_EQ(field[g.index(Cell{ 2, 0 })], -1); // still walled
    }

    section("pathfind.distanceFieldWeighted");
    {
        // 5x5, tile id 2 ("mud") costs 5 to enter; a mud column at x=2.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.fill(0, 1);
        for (int y = 0; y < 5; ++y) g.setTile(0, Cell{ 2, y }, 2);
        CostFn cost = [](const TileGrid& gg, Cell, Cell to) -> float {
            return gg.tile(0, to) == 2 ? 5.0f : 1.0f;
        };
        auto field = distanceFieldWeighted(g, { Cell{ 0, 0 } }, alwaysPass, cost, Conn::Edge);
        CHECK_EQ(static_cast<int>(field.size()), 25);
        CHECK_EQ(field[g.index(Cell{ 0, 0 })], 0.0f);
        CHECK_EQ(field[g.index(Cell{ 1, 0 })], 1.0f);
        CHECK_EQ(field[g.index(Cell{ 2, 0 })], 6.0f);  // 2 grass + mud entry
        CHECK_EQ(field[g.index(Cell{ 3, 0 })], 7.0f);  // mud column must be crossed once
        CHECK_EQ(field[g.index(Cell{ 4, 4 })], 12.0f); // 7 grass steps + one mud crossing

        // Null cost == uniform BFS (float flavour).
        auto uni = distanceFieldWeighted(g, { Cell{ 0, 0 } }, alwaysPass, nullptr, Conn::Edge);
        auto bfs = distanceField(g, { Cell{ 0, 0 } }, alwaysPass, Conn::Edge);
        bool agree = true;
        for (size_t i = 0; i < uni.size(); ++i)
            if (static_cast<int>(uni[i]) != bfs[i]) agree = false;
        CHECK(agree);

        // Impassable source still seeds 0 and spreads (matches distanceField).
        TileGrid gb(5, 5, Topology::Square, { "ground" });
        gb.setFlag(Cell{ 2, 2 }, BLOCK, true);
        auto bf = distanceFieldWeighted(gb, { Cell{ 2, 2 } }, pass, nullptr, Conn::Edge);
        CHECK_EQ(bf[gb.index(Cell{ 2, 2 })], 0.0f);
        CHECK_EQ(bf[gb.index(Cell{ 3, 2 })], 1.0f);
    }

    section("pathfind.aStar.straight");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        auto path = aStar(g, Cell{ 0, 0 }, Cell{ 4, 0 }, alwaysPass, nullptr, Conn::Edge);
        CHECK_EQ(static_cast<int>(path.size()), 5);
        CHECK(path.front() == (Cell{ 0, 0 }));
        CHECK(path.back() == (Cell{ 4, 0 }));
        CHECK(contiguous(g, path, Conn::Edge));
    }

    section("pathfind.aStar.aroundWall");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        for (int y = 0; y < 4; ++y) g.setFlag(Cell{ 2, y }, BLOCK, true);
        auto path = aStar(g, Cell{ 0, 0 }, Cell{ 4, 0 }, pass, nullptr, Conn::Edge);
        CHECK(path.size() > 0);
        CHECK(path.front() == (Cell{ 0, 0 }));
        CHECK(path.back() == (Cell{ 4, 0 }));
        CHECK(contiguous(g, path, Conn::Edge));
        // Optimal length matches the distance field (12 steps = 13 cells).
        CHECK_EQ(static_cast<int>(path.size()), 13);
        // No path cell is blocked.
        bool clean = true;
        for (Cell c : path) if (g.hasFlag(c, BLOCK)) clean = false;
        CHECK(clean);
    }

    section("pathfind.aStar.noPath");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        for (int y = 0; y < 5; ++y) g.setFlag(Cell{ 2, y }, BLOCK, true);
        auto path = aStar(g, Cell{ 0, 0 }, Cell{ 4, 0 }, pass, nullptr, Conn::Edge);
        CHECK_EQ(static_cast<int>(path.size()), 0);
    }

    section("pathfind.aStar.edgeCases");
    {
        TileGrid g(5, 5, Topology::Square, { "ground" });
        // start == goal -> {start}
        auto same = aStar(g, Cell{ 2, 2 }, Cell{ 2, 2 }, alwaysPass);
        CHECK_EQ(static_cast<int>(same.size()), 1);
        CHECK(same.front() == (Cell{ 2, 2 }));

        // OOB endpoints -> empty
        CHECK_EQ(static_cast<int>(aStar(g, Cell{ -1, 0 }, Cell{ 2, 2 }, alwaysPass).size()), 0);
        CHECK_EQ(static_cast<int>(aStar(g, Cell{ 0, 0 }, Cell{ 9, 9 }, alwaysPass).size()), 0);

        // Impassable goal -> empty.
        g.setFlag(Cell{ 4, 4 }, BLOCK, true);
        CHECK_EQ(static_cast<int>(aStar(g, Cell{ 0, 0 }, Cell{ 4, 4 }, pass).size()), 0);
    }

    section("pathfind.aStar.startImpassable");
    {
        // Start blocked but goal reachable: must still path OUT.
        TileGrid g(5, 5, Topology::Square, { "ground" });
        g.setFlag(Cell{ 0, 0 }, BLOCK, true);
        auto path = aStar(g, Cell{ 0, 0 }, Cell{ 3, 0 }, pass, nullptr, Conn::Edge);
        CHECK(path.size() > 0);
        CHECK(path.front() == (Cell{ 0, 0 }));
        CHECK(path.back() == (Cell{ 3, 0 }));
        CHECK(contiguous(g, path, Conn::Edge));
    }

    section("pathfind.aStar.vertexDiagonal");
    {
        // Vertex connectivity: diagonal shortcut to (3,3) is 3 steps (4 cells).
        TileGrid g(5, 5, Topology::Square, { "ground" });
        auto path = aStar(g, Cell{ 0, 0 }, Cell{ 3, 3 }, alwaysPass, nullptr, Conn::Vertex);
        CHECK_EQ(static_cast<int>(path.size()), 4);
        CHECK(path.front() == (Cell{ 0, 0 }));
        CHECK(path.back() == (Cell{ 3, 3 }));
        CHECK(contiguous(g, path, Conn::Vertex));
    }
}

} // namespace bro::tile::test
