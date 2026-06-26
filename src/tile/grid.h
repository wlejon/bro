#pragma once

// grid.h — TileGrid: the pure tile data model.
//
// A bounded-but-growable rectangular store of cells. Each cell carries:
//   - one tile id per named LAYER (uint16, 0 == empty),
//   - a signed elevation level (int16),
//   - a 32-bit flag bitmask (meanings defined by the caller: walkable,
//     buildable, tilled, ... — the core assigns no semantics).
//
// Storage is dense row-major (index = y*width + x) and is shared by both
// topologies — Hex grids use the same rectangle, interpreting (x, y) as
// odd-r offset coords (see coord.h). Topology only matters to the query
// layers (region/autotile/pathfind), which read it back via topology().
//
// Out-of-bounds policy:
//   - readers (tile/elevation/flags/hasFlag) return a ZERO default for OOB
//     cells, so edge queries (autotile masks at the border) need no guards;
//   - writers (setTile/setElevation/setFlags/...) are NO-OPS for OOB cells.
// Use inBounds() when the distinction matters.

#include "tile/coord.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::tile {

class TileGrid {
public:
    // Construct a width x height grid with the given topology and named layers
    // (at least one layer required; order is the layer index order). All cells
    // start empty (tile 0, elevation 0, flags 0).
    TileGrid(int width, int height, Topology topo, std::vector<std::string> layerNames);

    // ---- dimensions / identity ------------------------------------------
    int width() const { return w_; }
    int height() const { return h_; }
    Topology topology() const { return topo_; }
    int cellCount() const { return w_ * h_; }

    // ---- layers ---------------------------------------------------------
    int layerCount() const { return static_cast<int>(layers_.size()); }
    // Index of a layer by name, or -1 if there is no such layer.
    int layerIndex(const std::string& name) const;
    const std::string& layerName(int layer) const { return layerNames_[layer]; }

    // ---- bounds + indexing ----------------------------------------------
    bool inBounds(Cell c) const { return c.x >= 0 && c.y >= 0 && c.x < w_ && c.y < h_; }
    // Row-major linear index. UNDEFINED for OOB cells — guard with inBounds().
    size_t index(Cell c) const { return static_cast<size_t>(c.y) * w_ + c.x; }
    Cell cellOf(size_t idx) const {
        return Cell{ static_cast<int>(idx % w_), static_cast<int>(idx / w_) };
    }

    // ---- tiles ----------------------------------------------------------
    uint16_t tile(int layer, Cell c) const;            // 0 if OOB / empty
    void setTile(int layer, Cell c, uint16_t id);      // no-op if OOB

    // ---- elevation ------------------------------------------------------
    int16_t elevation(Cell c) const;                   // 0 if OOB
    void setElevation(Cell c, int16_t level);          // no-op if OOB

    // ---- flags (bitmask) ------------------------------------------------
    uint32_t flags(Cell c) const;                      // 0 if OOB
    void setFlags(Cell c, uint32_t mask);              // overwrite; no-op if OOB
    void setFlag(Cell c, uint32_t bit, bool on);       // set/clear one or more bits
    bool hasFlag(Cell c, uint32_t bit) const;          // (flags & bit) == bit

    // ---- bulk authoring -------------------------------------------------
    void fill(int layer, uint16_t id);                 // whole layer
    // Inclusive rectangle between corners a and b (any winding), clamped to bounds.
    void fillRect(int layer, Cell a, Cell b, uint16_t id);

    // ---- growth ---------------------------------------------------------
    // Resize to newWidth x newHeight. Existing content is copied to the new
    // store shifted by (offsetX, offsetY); cells that fall outside the new
    // bounds are dropped, newly exposed cells are empty. newWidth/newHeight
    // must be >= 1. Layer set and topology are preserved.
    void grow(int newWidth, int newHeight, int offsetX = 0, int offsetY = 0);

    // ---- raw access (serialize / meshing) -------------------------------
    // Contiguous row-major data for a layer / the elevation / the flag planes.
    // Lengths are all cellCount(). Exposed const for serialize + the future
    // renderer; mutate through the typed setters above.
    const std::vector<uint16_t>& layerData(int layer) const { return layers_[layer]; }
    const std::vector<int16_t>& elevationData() const { return elevation_; }
    const std::vector<uint32_t>& flagData() const { return flags_; }
    const std::vector<std::string>& layerNames() const { return layerNames_; }

private:
    int w_ = 0;
    int h_ = 0;
    Topology topo_ = Topology::Square;
    std::vector<std::string> layerNames_;
    std::vector<std::vector<uint16_t>> layers_;  // [layer][cell]
    std::vector<int16_t> elevation_;             // [cell]
    std::vector<uint32_t> flags_;                // [cell]
};

} // namespace bro::tile
