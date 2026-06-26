#pragma once

// autotile.h — neighbour bitmask -> tile variant index.
//
// This is what turns "a grid of the same tile id" into something that reads as
// authored: each cell picks an edge/corner variant based on which of its
// neighbours belong to the same visual "family" (same path, same water body,
// joining fence, …). The core computes only the INTEGER variant index from the
// neighbourhood; mapping an index to a concrete mesh/sprite/UV is the renderer's
// job. Family membership is a predicate so callers define what joins to what.
//
// Two encodings are provided:
//   - direction masks (one bit per neighbour direction) — works for Square and
//     Hex, bit order matches coord.h's canonical neighbour order;
//   - the classic 47-tile "blob" reduction — Square 8-neighbour only.

#include "tile/coord.h"
#include "tile/grid.h"

#include <cstdint>
#include <functional>

namespace bro::tile {

// Does the neighbour cell belong to the same family as the center? Called with
// possibly-OOB cells; OOB is conventionally treated as "not a member" by the
// factory predicates below, but a custom predicate may treat the border as a
// member (so a path doesn't fray at the map edge) — that's the caller's choice.
using FamilyFn = std::function<bool(const TileGrid&, Cell)>;

// Edge-only mask: one bit per EDGE neighbour, set when that neighbour passes
// `fam`. Square Edge -> 4 bits (E,N,W,S = bits 0..3). Hex -> 6 bits in canonical
// hex direction order. Square Vertex collapses to the same 4 edge bits.
uint8_t edgeMask(const TileGrid& g, Cell c, const FamilyFn& fam);

// Full 8-neighbour mask for Square (bit i set when neighbor i passes `fam`,
// using the Vertex direction order E,NE,N,NW,W,SW,S,SE = bits 0..7).
// Defined for Square only; returns edgeMask() for Hex.
uint8_t blobMask(const TileGrid& g, Cell c, const FamilyFn& fam);

// Reduce a full 8-bit blobMask to the canonical 47-tile blob variant index
// [0..46]. A diagonal only "counts" when both of its shared edges are also
// members (the standard rule that removes the impossible/duplicate cases from
// 256 down to 47). Square only.
int blob47(uint8_t blobMask8);

// Wang 2-edge corner index [0..15]: one bit per CORNER (NE,SE,SW,NW = bits 0..3)
// set when that corner's diagonal neighbour passes `fam`. For corner-based
// terrain blending. Square only.
uint8_t wangCorners(const TileGrid& g, Cell c, const FamilyFn& fam);

// ---- common family predicate factories ----------------------------------

// Family = "tile on `layer` equals `id`". OOB cells are NOT members.
FamilyFn familyTile(int layer, uint16_t id);

// Family = "tile on `layer` is non-empty (!= 0)". OOB cells are NOT members.
FamilyFn familyNonEmpty(int layer);

} // namespace bro::tile
