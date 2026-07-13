#pragma once

// serialize.h — versioned byte (de)serialization of a TileGrid.
//
// A compact, self-describing, little-endian binary blob: magic + version +
// topology + dimensions + layer names + the layer/elevation/flag planes. Round
// trips exactly (serialize -> deserialize -> serialize is byte-identical).
// Format is forward-guarded by a version field so the renderer/map tools can
// persist maps to disk and reload them.
//
// Layout (all integers little-endian):
//   [0..3]   magic   = 'B','T','I','L'
//   [4]      version = 1
//   [5]      topology (0 Square, 1 Hex)
//   [6]      layerCount
//   [7]      reserved (0)
//   int32    width
//   int32    height
//   per layer: uint16 nameLen, then nameLen UTF-8 bytes
//   uint16[width*height*layerCount]  layer planes, layer-major then row-major
//   int16 [width*height]             elevation plane
//   uint32[width*height]             flag plane

#include "tile/grid.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bro::tile {

// Serialize `g` to a byte buffer (never empty for a valid grid).
std::vector<uint8_t> serialize(const TileGrid& g);

// Parse a buffer produced by serialize(). Returns nullopt on bad magic,
// unknown version, or truncated/oversized data.
//
// Never throws, including on hostile input: the grid is sized from the actual
// payload length, not from the header's width/height claim, so a short blob
// declaring 2^31 x 2^31 cells is rejected rather than attempted.
std::optional<TileGrid> deserialize(const std::vector<uint8_t>& bytes);

} // namespace bro::tile
