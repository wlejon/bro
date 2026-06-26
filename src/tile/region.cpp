// region.cpp — flood-fill + connected-component selection over a TileGrid.

#include "tile/region.h"

#include <vector>

namespace bro::tile {

// Internal: BFS flood from `seed` into the shared `visited` plane. Appends the
// reached cells (BFS order, seed first) to `out`. Assumes seed is in-bounds,
// match-passing, and not yet visited.
static void floodFrom(const TileGrid& g, Cell seed, const MatchFn& match, Conn conn,
                      std::vector<char>& visited, std::vector<Cell>& out) {
    const Topology topo = g.topology();
    size_t head = out.size();
    visited[g.index(seed)] = 1;
    out.push_back(seed);
    while (head < out.size()) {
        Cell c = out[head++];
        Neighbors nb = neighbors(topo, c, conn);
        for (const Cell& n : nb) {
            if (!g.inBounds(n)) continue;
            size_t i = g.index(n);
            if (visited[i]) continue;
            if (!match(g, n)) continue;
            visited[i] = 1;
            out.push_back(n);
        }
    }
}

std::vector<Cell> floodFill(const TileGrid& g, Cell seed, const MatchFn& match, Conn conn) {
    std::vector<Cell> out;
    if (!g.inBounds(seed) || !match(g, seed)) return out;
    std::vector<char> visited(static_cast<size_t>(g.cellCount()), 0);
    floodFrom(g, seed, match, conn, visited, out);
    return out;
}

std::vector<std::vector<Cell>> components(const TileGrid& g, const MatchFn& match, Conn conn) {
    std::vector<std::vector<Cell>> result;
    const int n = g.cellCount();
    if (n <= 0) return result;
    std::vector<char> visited(static_cast<size_t>(n), 0);
    for (int idx = 0; idx < n; ++idx) {
        if (visited[idx]) continue;
        Cell c = g.cellOf(static_cast<size_t>(idx));
        if (!match(g, c)) continue;
        std::vector<Cell> comp;
        floodFrom(g, c, match, conn, visited, comp);
        result.push_back(std::move(comp));
    }
    return result;
}

MatchFn matchSameTile(int layer, uint16_t id) {
    return [layer, id](const TileGrid& g, Cell c) { return g.tile(layer, c) == id; };
}

MatchFn matchTile(int layer, uint16_t id) {
    return [layer, id](const TileGrid& g, Cell c) { return g.tile(layer, c) == id; };
}

MatchFn matchFlag(uint32_t mask) {
    return [mask](const TileGrid& g, Cell c) { return g.hasFlag(c, mask); };
}

} // namespace bro::tile
