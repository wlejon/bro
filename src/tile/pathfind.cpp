// pathfind.cpp — pure grid search over a TileGrid (BFS field + A*).

#include "tile/pathfind.h"

#include <algorithm>
#include <queue>
#include <vector>

namespace bro::tile {

std::vector<int> distanceField(const TileGrid& g, const std::vector<Cell>& sources,
                               const PassFn& pass, Conn conn) {
    const int n = g.cellCount();
    std::vector<int> dist(static_cast<size_t>(n), -1);
    if (n <= 0) return dist;
    const Topology topo = g.topology();

    std::vector<int> frontier; // row-major indices at current distance
    for (const Cell& s : sources) {
        if (!g.inBounds(s)) continue;
        size_t i = g.index(s);
        if (dist[i] != 0) {
            dist[i] = 0;
            frontier.push_back(static_cast<int>(i));
        }
    }

    // BFS via index queue (FIFO preserves the uniform-step invariant).
    std::queue<int> q;
    for (int i : frontier) q.push(i);
    while (!q.empty()) {
        int ci = q.front(); q.pop();
        Cell c = g.cellOf(static_cast<size_t>(ci));
        int nd = dist[ci] + 1;
        Neighbors nb = neighbors(topo, c, conn);
        for (const Cell& nbc : nb) {
            if (!g.inBounds(nbc)) continue;
            size_t i = g.index(nbc);
            if (dist[i] != -1) continue;
            if (!pass(g, nbc)) continue;
            dist[i] = nd;
            q.push(static_cast<int>(i));
        }
    }
    return dist;
}

std::vector<float> distanceFieldWeighted(const TileGrid& g, const std::vector<Cell>& sources,
                                         const PassFn& pass, const CostFn& cost, Conn conn) {
    const int n = g.cellCount();
    std::vector<float> dist(static_cast<size_t>(n), -1.0f);
    if (n <= 0) return dist;
    const Topology topo = g.topology();

    // Min-heap Dijkstra; tie-break by lower row-major index for determinism.
    struct Node {
        float d;
        int index;
    };
    struct Cmp {
        bool operator()(const Node& a, const Node& b) const {
            if (a.d != b.d) return a.d > b.d;
            return a.index > b.index;
        }
    };
    std::priority_queue<Node, std::vector<Node>, Cmp> open;

    for (const Cell& s : sources) {
        if (!g.inBounds(s)) continue;
        size_t i = g.index(s);
        if (dist[i] != 0.0f) {
            dist[i] = 0.0f;
            open.push(Node{ 0.0f, static_cast<int>(i) });
        }
    }

    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        int ci = cur.index;
        if (cur.d > dist[ci]) continue; // stale entry
        Cell c = g.cellOf(static_cast<size_t>(ci));
        Neighbors nb = neighbors(topo, c, conn);
        for (const Cell& nbc : nb) {
            if (!g.inBounds(nbc)) continue;
            if (!pass(g, nbc)) continue;
            size_t i = g.index(nbc);
            float step = cost ? cost(g, c, nbc) : 1.0f;
            float nd = cur.d + step;
            if (dist[i] < 0.0f || nd < dist[i]) {
                dist[i] = nd;
                open.push(Node{ nd, static_cast<int>(i) });
            }
        }
    }
    return dist;
}

std::vector<Cell> aStar(const TileGrid& g, Cell start, Cell goal, const PassFn& pass,
                        const CostFn& cost, Conn conn) {
    std::vector<Cell> path;
    if (!g.inBounds(start) || !g.inBounds(goal)) return path;
    if (!pass(g, goal)) return path;

    const Topology topo = g.topology();
    const int n = g.cellCount();
    const float INF = 1e30f;
    std::vector<float> gScore(static_cast<size_t>(n), INF);
    std::vector<int> cameFrom(static_cast<size_t>(n), -1);

    // Open-set entry; min-heap by f, tie-break by lower row-major index.
    struct Node {
        float f;
        int index;
    };
    struct Cmp {
        bool operator()(const Node& a, const Node& b) const {
            if (a.f != b.f) return a.f > b.f;
            return a.index > b.index;
        }
    };
    std::priority_queue<Node, std::vector<Node>, Cmp> open;

    const int startIdx = static_cast<int>(g.index(start));
    const int goalIdx = static_cast<int>(g.index(goal));
    gScore[startIdx] = 0.0f;
    float h0 = static_cast<float>(distance(topo, start, goal, conn));
    open.push(Node{ h0, startIdx });

    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        int ci = cur.index;
        Cell c = g.cellOf(static_cast<size_t>(ci));
        // Skip stale entries (a better path was already popped).
        float expectedF = gScore[ci] + static_cast<float>(distance(topo, c, goal, conn));
        if (cur.f > expectedF) continue;

        if (ci == goalIdx) {
            // Reconstruct start..goal.
            for (int at = goalIdx; at != -1; at = cameFrom[at])
                path.push_back(g.cellOf(static_cast<size_t>(at)));
            std::reverse(path.begin(), path.end());
            return path;
        }

        Neighbors nb = neighbors(topo, c, conn);
        for (const Cell& nbc : nb) {
            if (!g.inBounds(nbc)) continue;
            if (!pass(g, nbc)) continue;
            size_t i = g.index(nbc);
            float step = cost ? cost(g, c, nbc) : 1.0f;
            float tentative = gScore[ci] + step;
            if (tentative < gScore[i]) {
                gScore[i] = tentative;
                cameFrom[i] = ci;
                float f = tentative + static_cast<float>(distance(topo, nbc, goal, conn));
                open.push(Node{ f, static_cast<int>(i) });
            }
        }
    }
    return path; // unreachable
}

PassFn passUnlessFlag(uint32_t blockMask) {
    // Blocked if ANY bit of blockMask is set (header contract), so a cell is
    // passable only when it shares no bits with the mask.
    return [blockMask](const TileGrid& g, Cell c) { return (g.flags(c) & blockMask) == 0; };
}

} // namespace bro::tile
