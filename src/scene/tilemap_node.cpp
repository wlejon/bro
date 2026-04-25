#include "scene/tilemap_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

#include <stb_image.h>

#include <cmath>

namespace bro::scene {

TilemapNode::TilemapNode(const std::string& name) : SceneNode(name) {}

void TilemapNode::setMapSize(int columns, int rows) {
    if (columns < 0) columns = 0;
    if (rows < 0)    rows = 0;
    cols_ = columns;
    rows_ = rows;
    size_t total = static_cast<size_t>(columns) * static_cast<size_t>(rows);
    for (auto& layer : layers_) {
        layer.data.assign(total, 0);
    }
}

void TilemapNode::setTileset(const std::string& path, int srcTileW, int srcTileH, int srcColumns) {
    tilesetPath_ = path;
    srcTileW_ = (srcTileW > 0) ? srcTileW : 32;
    srcTileH_ = (srcTileH > 0) ? srcTileH : 32;
    srcColumns_ = srcColumns;
    texLoaded_ = false;
    texPixels_.clear();
    texW_ = texH_ = 0;
}

int TilemapNode::addLayer(const std::string& name) {
    int idx = layerIndex(name);
    if (idx >= 0) return idx;
    Layer layer;
    layer.name = name;
    layer.data.assign(static_cast<size_t>(cols_) * static_cast<size_t>(rows_), 0);
    layers_.push_back(std::move(layer));
    return static_cast<int>(layers_.size()) - 1;
}

int TilemapNode::layerIndex(const std::string& name) const {
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (layers_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const std::string& TilemapNode::layerName(int idx) const {
    static const std::string empty;
    if (idx < 0 || idx >= (int)layers_.size()) return empty;
    return layers_[idx].name;
}

void TilemapNode::setLayerData(int layer, const uint16_t* data, size_t count) {
    ensureLayer(layer);
    auto& dst = layers_[layer].data;
    size_t total = static_cast<size_t>(cols_) * static_cast<size_t>(rows_);
    dst.assign(total, 0);
    size_t copyN = (count < total) ? count : total;
    if (data) {
        for (size_t i = 0; i < copyN; ++i) dst[i] = data[i];
    }
}

void TilemapNode::ensureLayer(int idx) {
    while ((int)layers_.size() <= idx) {
        Layer layer;
        layer.data.assign(static_cast<size_t>(cols_) * static_cast<size_t>(rows_), 0);
        layers_.push_back(std::move(layer));
    }
}

void TilemapNode::setTile(int col, int row, uint16_t tileIndex, int layer) {
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_) return;
    ensureLayer(layer);
    layers_[layer].data[static_cast<size_t>(row) * cols_ + col] = tileIndex;
}

uint16_t TilemapNode::getTile(int col, int row, int layer) const {
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_) return 0;
    if (layer < 0 || layer >= (int)layers_.size()) return 0;
    return layers_[layer].data[static_cast<size_t>(row) * cols_ + col];
}

void TilemapNode::tileAtLocal(float localX, float localY, int& outCol, int& outRow) const {
    outCol = (tileW_ > 0) ? static_cast<int>(std::floor(localX / static_cast<float>(tileW_))) : 0;
    outRow = (tileH_ > 0) ? static_cast<int>(std::floor(localY / static_cast<float>(tileH_))) : 0;
}

bool TilemapNode::tileAtWorld(float worldX, float worldY, int& outCol, int& outRow) const {
    // Build the inverse of the world matrix's 2D affine part. We only support
    // uniform scale + Z rotation for the inversion; tilemaps with non-affine
    // 3D transforms aren't a real-world use case.
    const auto& wm = worldMatrix();
    float a = wm.m[0][0], b = wm.m[0][1];
    float c = wm.m[1][0], d = wm.m[1][1];
    float tx = wm.m[3][0], ty = wm.m[3][1];
    float det = a * d - b * c;
    if (det == 0.0f) return false;
    float inv = 1.0f / det;
    float dx = worldX - tx;
    float dy = worldY - ty;
    float lx = ( d * dx - c * dy) * inv;
    float ly = (-b * dx + a * dy) * inv;
    tileAtLocal(lx, ly, outCol, outRow);
    if (outCol < 0 || outRow < 0 || outCol >= cols_ || outRow >= rows_) return false;
    return true;
}

void TilemapNode::loadTilesetIfNeeded() const {
    if (texLoaded_ || tilesetPath_.empty()) return;
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(tilesetPath_.c_str(), &w, &h, &channels, 4);
    if (data) {
        texW_ = w; texH_ = h;
        texPixels_.assign(data, data + w * h * 4);
        stbi_image_free(data);
    }
    texLoaded_ = true;
}

void TilemapNode::onRender(SceneGraph& graph) {
    if (cols_ <= 0 || rows_ <= 0 || layers_.empty()) return;
    auto* cs = graph.canvasScene();
    if (!cs) return;
    if (tilesetPath_.empty()) return;

    loadTilesetIfNeeded();
    if (texPixels_.empty() || texW_ <= 0 || texH_ <= 0) return;

    int srcCols = (srcColumns_ > 0) ? srcColumns_ : (texW_ / srcTileW_);
    if (srcCols <= 0) return;

    const auto& wm = worldMatrix();
    cs->save();
    cs->setTransform(wm.m[0][0], wm.m[0][1], wm.m[1][0], wm.m[1][1], wm.m[3][0], wm.m[3][1]);

    // Layers render bottom-to-top (index 0 first).
    for (const auto& layer : layers_) {
        const auto& data = layer.data;
        for (int row = 0; row < rows_; ++row) {
            for (int col = 0; col < cols_; ++col) {
                uint16_t v = data[static_cast<size_t>(row) * cols_ + col];
                if (v == 0) continue;
                int idx = static_cast<int>(v) - 1;     // 1-based -> 0-based
                int sx = (idx % srcCols) * srcTileW_;
                int sy = (idx / srcCols) * srcTileH_;
                cs->drawImage(texPixels_.data(), texW_, texH_,
                              static_cast<float>(sx), static_cast<float>(sy),
                              static_cast<float>(srcTileW_), static_cast<float>(srcTileH_),
                              static_cast<float>(col * tileW_),
                              static_cast<float>(row * tileH_),
                              static_cast<float>(tileW_),
                              static_cast<float>(tileH_));
            }
        }
    }

    cs->restore();
}

} // namespace bro::scene
