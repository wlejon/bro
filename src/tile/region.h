#pragma once

// region.h — flood-fill + connected-component selection over a TileGrid.
//
// These are the "select a contiguous area" primitives: pick out a pen, a
// field, a lake, the walkable interior, etc. Selection is predicate-driven so
// callers decide what "belongs" means (same tile id on a layer, a flag set, an
// elevation band, …). Connectivity follows the grid's topology; for Square the
// caller picks Edge (4-way) or Vertex (8-way).

#include "tile/coord.h"
#include "tile/grid.h"

#include <functional>
#include <vector>

namespace bro::tile {

// Predicate: does this cell belong to the selection? Always called with
// in-bounds cells.
using MatchFn = std::function<bool(const TileGrid&, Cell)>;

// All cells reachable from `seed` through `match`-passing cells (4/6/8-connected
// per topology + conn). Empty if the seed itself fails `match` or is OOB.
// Order is deterministic (BFS from seed).
std::vector<Cell> floodFill(const TileGrid& g, Cell seed, const MatchFn& match,
                            Conn conn = Conn::Edge);

// Every maximal connected component of `match`-passing cells in the grid.
// Components are returned in scan order (top-left cell first); each component's
// cells are in BFS order. Useful for "how many separate fields are there".
std::vector<std::vector<Cell>> components(const TileGrid& g, const MatchFn& match,
                                          Conn conn = Conn::Edge);

// ---- common predicate factories -----------------------------------------

// Match cells whose tile on `layer` equals the seed cell's tile on that layer.
// (Bind the seed's value once and compare — handy for "same terrain as here".)
MatchFn matchSameTile(int layer, uint16_t id);

// Match cells whose tile on `layer` is exactly `id`.
MatchFn matchTile(int layer, uint16_t id);

// Match cells that have ALL bits of `mask` set in their flags.
MatchFn matchFlag(uint32_t mask);

} // namespace bro::tile
