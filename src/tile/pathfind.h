#pragma once

// pathfind.h — pure grid search over a TileGrid.
//
// This is the CELL-GRID search layer: BFS distance fields and A* over tile
// cells. It is deliberately independent of the 3D navmesh (brogameagent /
// bro.ai.game) — that owns continuous-space steering for agents in a scene;
// this owns cheap, deterministic, integer grid queries (influence maps,
// "nearest source", grid routes for tools/headless logic, reachability tests).
//
// Passability + step cost are predicates so callers decide what blocks and what
// a step costs (terrain, flags, elevation deltas). Connectivity follows the
// grid topology (Square: Edge/Vertex; Hex: 6-way).

#include "tile/coord.h"
#include "tile/grid.h"

#include <functional>
#include <vector>

namespace bro::tile {

// Can an agent stand on / enter this cell? Called with in-bounds cells.
using PassFn = std::function<bool(const TileGrid&, Cell)>;

// Cost of stepping from `from` to the adjacent `to` (both in-bounds, passable).
// Return a value >= 1. If null is passed where a CostFn is accepted, every step
// costs 1 (uniform).
using CostFn = std::function<float(const TileGrid&, Cell from, Cell to)>;

// Multi-source BFS. Returns a row-major field (length g.cellCount()): the step
// distance from each cell to the NEAREST source through passable cells, or -1
// for unreachable / impassable / OOB cells. Sources need not be passable
// themselves (a source on an impassable cell still seeds distance 0 and its
// passable neighbours expand from it). Uniform step count — for weighted
// fields use distanceFieldWeighted().
std::vector<int> distanceField(const TileGrid& g, const std::vector<Cell>& sources,
                               const PassFn& pass, Conn conn = Conn::Edge);

// Weighted multi-source field (Dijkstra): cheapest path cost from each cell to
// the nearest source, where entering a cell costs `cost` (>= 1 per step, same
// contract as aStar). -1 for unreachable / impassable / OOB. A null cost is
// uniform (equivalent to distanceField, but float). Source-passability rule
// matches distanceField.
std::vector<float> distanceFieldWeighted(const TileGrid& g, const std::vector<Cell>& sources,
                                         const PassFn& pass, const CostFn& cost = nullptr,
                                         Conn conn = Conn::Edge);

// A* shortest path from `start` to `goal` inclusive of both endpoints. Returns
// an empty vector if no path exists, if either endpoint is OOB, or if `goal`
// is impassable. `start` is allowed to be impassable (you can path OUT of a
// blocked cell). The heuristic is the topology distance(), which is admissible
// for any CostFn with steps >= 1.
std::vector<Cell> aStar(const TileGrid& g, Cell start, Cell goal, const PassFn& pass,
                        const CostFn& cost = nullptr, Conn conn = Conn::Edge);

// ---- common predicate factories -----------------------------------------

// Passable = "cell does NOT have any bit of `blockMask` set in its flags".
PassFn passUnlessFlag(uint32_t blockMask);

// Passable = "tile on `layer` is one the caller allows" is too open-ended to
// bake; compose your own PassFn for tile-id rules. The flag-based factory above
// covers the common walkable-bit case.

} // namespace bro::tile
