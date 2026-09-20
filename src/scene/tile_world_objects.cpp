#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "scene/instanced_mesh_node.h"

#include <cmath>
#include <utility>
#include <vector>

namespace bro::scene {

int TileWorld::addObjectKind(bromesh::MeshData&& mesh, const ObjectStyle& style) {
    auto* g = graph();
    if (!g || !root_ || mesh.empty()) return -1;

    auto* node = g->createInstancedMesh("tile-objects");
    root_->addChild(node);
    attachShadeMap(node);
    node->setMesh(std::move(mesh));
    node->setColor(style.color[0], style.color[1], style.color[2], style.color[3]);
    node->setRoughness(style.roughness);
    node->setMetallic(style.metallic);
    node->setDoubleSided(style.doubleSided);
    node->setAlphaCutoff(style.alphaCutoff);
    node->setCastsShadow(style.castsShadow);
    node->setAtlasGrid(style.atlasCols, style.atlasRows);
    if (!style.texPixels.empty() && style.texWidth > 0 && style.texHeight > 0)
        node->setBaseColorTexture(style.texWidth, style.texHeight, style.texPixels.data());

    objectKinds_.push_back(ObjectKind{});
    objectKinds_.back().node = node;
    return static_cast<int>(objectKinds_.size()) - 1;
}

int TileWorld::addObject(int kind, int x, int y, const ObjectPlacement& p) {
    if (kind < 0 || kind >= static_cast<int>(objectKinds_.size())) return -1;
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return -1;
    ObjectKind& k = objectKinds_[kind];
    k.placements.push_back(p);
    k.cellX.push_back(x);
    k.cellY.push_back(y);
    k.dirty = true;
    return static_cast<int>(k.placements.size()) - 1;
}

void TileWorld::clearObjects(int kind) {
    auto clearOne = [](ObjectKind& k) {
        k.placements.clear(); k.cellX.clear(); k.cellY.clear(); k.dirty = true;
    };
    if (kind < 0) { for (auto& k : objectKinds_) clearOne(k); return; }
    if (kind < static_cast<int>(objectKinds_.size())) clearOne(objectKinds_[kind]);
}

int TileWorld::objectCount(int kind) const {
    if (kind < 0 || kind >= static_cast<int>(objectKinds_.size())) return 0;
    return static_cast<int>(objectKinds_[kind].placements.size());
}

void TileWorld::rebuildObjectKind(ObjectKind& k) {
    k.dirty = false;
    if (!k.node) return;

    const size_t n = k.placements.size();
    const float cs = config_.cellSize;

    std::vector<float> inst(n * 16);
    for (size_t i = 0; i < n; ++i) {
        const ObjectPlacement& p = k.placements[i];
        const int x = k.cellX[i], y = k.cellY[i];

        // Cell-centre anchor in root-local space (root carries the origin).
        float px = 0, pz = 0;
        cellCenterLocal(x, y, px, pz);
        px += p.offsetX * cs;
        pz += p.offsetZ * cs;
        const float py = topY(x, y) + p.yOffset;

        const float s = p.scale;
        const float c = std::cos(p.yaw), sn = std::sin(p.yaw);

        float* o = inst.data() + i * 16;
        // Y-rotation * uniform scale (row-major 4x3; rows 0..2 carry the affine).
        o[ 0] = c * s;  o[ 1] = 0;  o[ 2] = sn * s;  o[ 3] = px;
        o[ 4] = 0;      o[ 5] = s;  o[ 6] = 0;       o[ 7] = py;
        o[ 8] = -sn * s; o[9] = 0;  o[10] = c * s;   o[11] = pz;
        // Per-instance tint; when the kind has an atlas, alpha carries the
        // variant cell index (matches setInstancesFromPosQuatScale's packing).
        o[12] = p.color[0]; o[13] = p.color[1]; o[14] = p.color[2];
        bool atlas = (k.node->atlasCols() > 1 || k.node->atlasRows() > 1);
        if (atlas) {
            float vi = static_cast<float>(p.variant);
            vi = vi < 0.0f ? 0.0f : (vi > 255.0f ? 255.0f : vi);
            o[15] = (vi + 0.5f) / 256.0f;
        } else {
            o[15] = p.color[3];
        }
    }
    k.node->setInstances(inst.data(), n);
}

void TileWorld::rebuildObjects() {
    for (auto& k : objectKinds_)
        if (k.dirty) rebuildObjectKind(k);
}

} // namespace bro::scene
