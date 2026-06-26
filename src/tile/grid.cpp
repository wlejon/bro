// grid.cpp — TileGrid implementation. See grid.h for the contract.
//
// Dense row-major planes: one uint16 tile plane per layer, one int16 elevation
// plane, one uint32 flag plane. OOB readers return zero; OOB writers no-op.

#include "tile/grid.h"

#include <algorithm>
#include <utility>

namespace bro::tile {

TileGrid::TileGrid(int width, int height, Topology topo, std::vector<std::string> layerNames)
    : w_(width < 1 ? 1 : width),
      h_(height < 1 ? 1 : height),
      topo_(topo),
      layerNames_(std::move(layerNames)) {
    // At least one layer required; synthesize a default if none was given.
    if (layerNames_.empty())
        layerNames_.emplace_back("layer0");

    const size_t cells = static_cast<size_t>(w_) * static_cast<size_t>(h_);
    layers_.assign(layerNames_.size(), std::vector<uint16_t>(cells, 0));
    elevation_.assign(cells, 0);
    flags_.assign(cells, 0);
}

int TileGrid::layerIndex(const std::string& name) const {
    for (size_t i = 0; i < layerNames_.size(); ++i)
        if (layerNames_[i] == name)
            return static_cast<int>(i);
    return -1;
}

// ---- tiles --------------------------------------------------------------

uint16_t TileGrid::tile(int layer, Cell c) const {
    if (!inBounds(c) || layer < 0 || layer >= static_cast<int>(layers_.size()))
        return 0;
    return layers_[layer][index(c)];
}

void TileGrid::setTile(int layer, Cell c, uint16_t id) {
    if (!inBounds(c) || layer < 0 || layer >= static_cast<int>(layers_.size()))
        return;
    layers_[layer][index(c)] = id;
}

// ---- elevation ----------------------------------------------------------

int16_t TileGrid::elevation(Cell c) const {
    if (!inBounds(c))
        return 0;
    return elevation_[index(c)];
}

void TileGrid::setElevation(Cell c, int16_t level) {
    if (!inBounds(c))
        return;
    elevation_[index(c)] = level;
}

// ---- flags --------------------------------------------------------------

uint32_t TileGrid::flags(Cell c) const {
    if (!inBounds(c))
        return 0;
    return flags_[index(c)];
}

void TileGrid::setFlags(Cell c, uint32_t mask) {
    if (!inBounds(c))
        return;
    flags_[index(c)] = mask;
}

void TileGrid::setFlag(Cell c, uint32_t bit, bool on) {
    if (!inBounds(c))
        return;
    uint32_t& f = flags_[index(c)];
    if (on)
        f |= bit;
    else
        f &= ~bit;
}

bool TileGrid::hasFlag(Cell c, uint32_t bit) const {
    if (!inBounds(c))
        return false;
    return (flags_[index(c)] & bit) == bit;
}

// ---- bulk authoring -----------------------------------------------------

void TileGrid::fill(int layer, uint16_t id) {
    if (layer < 0 || layer >= static_cast<int>(layers_.size()))
        return;
    std::fill(layers_[layer].begin(), layers_[layer].end(), id);
}

void TileGrid::fillRect(int layer, Cell a, Cell b, uint16_t id) {
    if (layer < 0 || layer >= static_cast<int>(layers_.size()))
        return;
    int x0 = std::min(a.x, b.x);
    int x1 = std::max(a.x, b.x);
    int y0 = std::min(a.y, b.y);
    int y1 = std::max(a.y, b.y);
    // Clamp to the inclusive valid range.
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, w_ - 1);
    y1 = std::min(y1, h_ - 1);
    if (x0 > x1 || y0 > y1)
        return; // entirely outside
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            layers_[layer][index(Cell{ x, y })] = id;
}

// ---- growth -------------------------------------------------------------

void TileGrid::grow(int newWidth, int newHeight, int offsetX, int offsetY) {
    int nw = newWidth < 1 ? 1 : newWidth;
    int nh = newHeight < 1 ? 1 : newHeight;
    const size_t ncells = static_cast<size_t>(nw) * static_cast<size_t>(nh);

    std::vector<std::vector<uint16_t>> newLayers(layers_.size(),
                                                 std::vector<uint16_t>(ncells, 0));
    std::vector<int16_t> newElev(ncells, 0);
    std::vector<uint32_t> newFlags(ncells, 0);

    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            int nx = x + offsetX;
            int ny = y + offsetY;
            if (nx < 0 || ny < 0 || nx >= nw || ny >= nh)
                continue; // dropped — outside the new bounds
            size_t oldIdx = static_cast<size_t>(y) * w_ + x;
            size_t newIdx = static_cast<size_t>(ny) * nw + nx;
            for (size_t L = 0; L < layers_.size(); ++L)
                newLayers[L][newIdx] = layers_[L][oldIdx];
            newElev[newIdx] = elevation_[oldIdx];
            newFlags[newIdx] = flags_[oldIdx];
        }
    }

    w_ = nw;
    h_ = nh;
    layers_ = std::move(newLayers);
    elevation_ = std::move(newElev);
    flags_ = std::move(newFlags);
    // topology + layer names preserved.
}

} // namespace bro::tile
