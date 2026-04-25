#pragma once

#include "scene/scene_node.h"
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

/// 2D tilemap node. Stores N layers of `columns x rows` tiles and renders
/// them via the CanvasScene drawImage path. Tile value 0 = empty (skipped);
/// 1..tileCount = 1-based index into the tileset grid.
///
/// Position of the tilemap is the SceneNode position; the camera transform
/// applied in SceneGraph::render() handles scrolling.
class TilemapNode : public SceneNode {
public:
    explicit TilemapNode(const std::string& name = "");

    Type type() const override { return Type::Tilemap; }
    void onRender(SceneGraph& graph) override;

    // --- Configuration ---

    /// Per-tile output size in node-local pixels.
    void setTileSize(int w, int h) { tileW_ = w; tileH_ = h; }
    int  tileWidth()  const { return tileW_; }
    int  tileHeight() const { return tileH_; }

    /// Map dimensions in tiles. Resizes all existing layers; new cells = 0.
    void setMapSize(int columns, int rows);
    int  columns() const { return cols_; }
    int  rows()    const { return rows_; }

    /// Tileset image (lazy-loaded from path) and its grid layout. `srcTileW`/
    /// `srcTileH` describe each cell in image pixels; `srcColumns` is the
    /// horizontal cell count. Pass 0 for srcColumns to default to whatever the
    /// image width allows after load.
    void setTileset(const std::string& path, int srcTileW, int srcTileH, int srcColumns);
    const std::string& tilesetPath() const { return tilesetPath_; }

    /// Add (or replace) a named layer. Returns the layer index.
    int addLayer(const std::string& name);
    int layerIndex(const std::string& name) const;
    int layerCount() const { return static_cast<int>(layers_.size()); }
    const std::string& layerName(int idx) const;

    /// Replace a layer's data (must match cols_ * rows_ entries; otherwise
    /// resized to fit, padded with 0s).
    void setLayerData(int layer, const uint16_t* data, size_t count);

    void setTile(int col, int row, uint16_t tileIndex, int layer = 0);
    uint16_t getTile(int col, int row, int layer = 0) const;

    /// Convert world-space (x, y) to grid {col, row}. Returns false if outside
    /// the map. Honors the node's transform (so rotated/scaled tilemaps work).
    bool tileAtWorld(float worldX, float worldY, int& outCol, int& outRow) const;

    /// Convert local-space (x, y) to grid coords. No bounds check unless
    /// explicitly requested via the optional outIn flag.
    void tileAtLocal(float localX, float localY, int& outCol, int& outRow) const;

private:
    void ensureLayer(int idx);
    void loadTilesetIfNeeded() const;

    int tileW_ = 32, tileH_ = 32;
    int cols_ = 0, rows_ = 0;

    struct Layer {
        std::string name;
        std::vector<uint16_t> data;
    };
    std::vector<Layer> layers_;

    // Tileset
    std::string tilesetPath_;
    int srcTileW_ = 32, srcTileH_ = 32;
    int srcColumns_ = 0;            // computed at load time if 0
    mutable std::vector<uint8_t> texPixels_;
    mutable int texW_ = 0, texH_ = 0;
    mutable bool texLoaded_ = false;
};

} // namespace bro::scene
