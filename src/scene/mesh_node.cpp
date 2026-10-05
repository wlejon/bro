#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/analysis/bbox.h>
#include <bromesh/manipulation/normals.h>

#include <algorithm>
#include <cstring>

namespace bro::scene {

MeshNode::MeshNode(const std::string& name) : SceneNode(name) {}

MeshNode::~MeshNode() = default;

// Auto-populate tangents for normal mapping when the source geometry has the
// prerequisites (UVs + normals) but no tangent stream. Cheap enough to run
// unconditionally; the shader only references tangents when a normal map is
// bound, so the cost is wasted only when neither the mesh nor the material
// uses normal maps. Kept in one place so both setMesh overloads behave the same.
static void ensureTangents(bromesh::MeshData& m) {
    if (m.hasUVs() && m.hasNormals() && !m.hasTangents())
        bromesh::generateTangents(m);
}

void MeshNode::setMesh(const bromesh::MeshData& mesh) {
    mesh_ = mesh;
    ensureTangents(mesh_);
    geometryGeneration_ = nextResourceGeneration();
    if (lods_.empty()) hasVertexColors_ = mesh_.hasColors();
    bvhDirty_ = true;
    recomputeBounds();
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void MeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    geometryGeneration_ = nextResourceGeneration();
    if (lods_.empty()) hasVertexColors_ = mesh_.hasColors();
    bvhDirty_ = true;
    recomputeBounds();
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void MeshNode::recomputeBounds() {
    // Union of the base mesh and every LOD level, so frustum/shadow culling
    // is conservative for whichever level is selected on any given frame.
    bool any = false;
    bromath::AABB3 acc{};
    auto merge = [&](const bromesh::MeshData& m) {
        if (m.empty()) return;
        bromath::AABB3 b = bromesh::computeBBox(m);
        if (!any) {
            acc = b;
            any = true;
            return;
        }
        acc.min = {std::min(acc.min.x, b.min.x),
                   std::min(acc.min.y, b.min.y),
                   std::min(acc.min.z, b.min.z)};
        acc.max = {std::max(acc.max.x, b.max.x),
                   std::max(acc.max.y, b.max.y),
                   std::max(acc.max.z, b.max.z)};
    };
    merge(mesh_);
    for (auto& e : lods_) merge(e.mesh);
    bounds_ = any ? acc : bromath::AABB3{};
}

void MeshNode::setLodMeshes(std::vector<LodLevel> levels) {
    if (asSkinnedMesh()) {
        LOG_WARN("setLodMeshes: not supported on skinned meshes (ignored)");
        return;
    }
    lods_.clear();
    lods_.reserve(levels.size());
    for (auto& lv : levels) {
        LodEntry e;
        e.mesh = std::move(lv.mesh);
        ensureTangents(e.mesh);
        e.maxDist = lv.maxDist;
        e.hasColors = e.mesh.hasColors();
        lods_.push_back(std::move(e));
    }
    std::stable_sort(lods_.begin(), lods_.end(),
                     [](const LodEntry& a, const LodEntry& b) {
                         return a.maxDist < b.maxDist;
                     });
    lodSelected_ = 0;
    hasVertexColors_ = lods_.empty() ? mesh_.hasColors() : lods_[0].hasColors;
    geometryGeneration_ = nextResourceGeneration();
    recomputeBounds();
    bumpChangeGeneration();  // rendered geometry changed
}

void MeshNode::selectLodByDistance(float d) {
    if (lods_.empty()) return;
    int sel = static_cast<int>(lods_.size()) - 1;   // clamp to coarsest
    for (int i = 0; i < static_cast<int>(lods_.size()); ++i) {
        if (d < lods_[i].maxDist) { sel = i; break; }
    }
    if (sel != lodSelected_) {
        lodSelected_ = sel;
        hasVertexColors_ = lods_[sel].hasColors;
        bumpChangeGeneration();  // shadow tiles hold the old silhouette
    }
}

const bromesh::MeshBVH& MeshNode::bvh() const {
    if (bvhDirty_) {
        bvh_ = bromesh::MeshBVH::build(mesh_);
        bvhDirty_ = false;
    }
    return bvh_;
}

void MeshNode::setBaseColorTexture(int width, int height, const uint8_t* rgba) {
    externalSceneProvider_ = nullptr;  // owned bytes win; drop the live link
    baseColorTex_.set(width, height, rgba);
}
void MeshNode::clearBaseColorTexture() {
    externalSceneProvider_ = nullptr;
    baseColorTex_.clear();
}

void MeshNode::setNormalTexture(int width, int height, const uint8_t* rgba) {
    normalTex_.set(width, height, rgba);
}
void MeshNode::clearNormalTexture() { normalTex_.clear(); }

void MeshNode::setMetallicRoughnessTexture(int width, int height, const uint8_t* rgba) {
    mrTex_.set(width, height, rgba);
}
void MeshNode::clearMetallicRoughnessTexture() { mrTex_.clear(); }

void MeshNode::setOcclusionTexture(int width, int height, const uint8_t* rgba) {
    aoTex_.set(width, height, rgba);
}
void MeshNode::clearOcclusionTexture() { aoTex_.clear(); }

void MeshNode::setEmissiveTexture(int width, int height, const uint8_t* rgba) {
    emissiveTex_.set(width, height, rgba);
}
void MeshNode::clearEmissiveTexture() { emissiveTex_.clear(); }

bool MeshNode::setCustomShaderTexture(const std::string& name, int width,
                                      int height, const float* data,
                                      bool mipmap, bool repeat,
                                      bool clampT, int channels) {
    const bool release = (width <= 0 || height <= 0 || !data);
    for (auto it = userTextures_.begin(); it != userTextures_.end(); ++it) {
        UserTexture& t = *it;
        if (t.name != name) continue;
        if (release) {
            // Dropping the slot frees the name for the budget; the renderer
            // releases its GPU copy when it no longer sees the name.
            userTextures_.erase(it);
            bumpChangeGeneration();
            return true;
        }
        // A full upload replaces the whole image, so sub-rect writes staged
        // against the OLD contents are superseded, and a 2D upload onto what
        // was an array slot turns it back into a plain 2D slot.
        t.subUpdates.clear();
        t.sliceUpdates.clear();
        t.layers = 0;
        t.data.assign(data, data + (size_t)width * (size_t)height * (size_t)channels);
        t.w = width;
        t.h = height;
        t.channels = channels;
        t.mipmap = mipmap;
        t.repeat = repeat;
        t.clampT = clampT;
        t.generation = nextResourceGeneration();
        bumpChangeGeneration();
        return true;
    }
    if (release) return true;   // releasing an unknown slot is a no-op
    if ((int)userTextures_.size() >= maxUserTextures()) return false;
    UserTexture t;
    t.name = name;
    t.data.assign(data, data + (size_t)width * (size_t)height * (size_t)channels);
    t.w = width;
    t.h = height;
    t.channels = channels;
    t.mipmap = mipmap;
    t.repeat = repeat;
    t.clampT = clampT;
    t.generation = nextResourceGeneration();
    userTextures_.push_back(std::move(t));
    bumpChangeGeneration();
    return true;
}

bool MeshNode::updateCustomShaderTexture(const std::string& name, int x, int y,
                                         int width, int height,
                                         const float* data) {
    if (width <= 0 || height <= 0 || !data) {
        LOG_WARN("updateShaderTexture('%s'): non-positive extent or no data "
                 "(ignored)", name.c_str());
        return false;
    }
    for (auto& t : userTextures_) {
        if (t.name != name) continue;
        if (t.layers > 0) {
            LOG_WARN("updateShaderTexture('%s'): an array slot takes whole "
                     "slices, not sub-rects (ignored)", name.c_str());
            return false;
        }
        if (t.w <= 0 || t.h <= 0) {
            LOG_WARN("updateShaderTexture('%s'): slot has no dimensions yet — "
                     "set the full texture first (ignored)", name.c_str());
            return false;
        }
        if (x < 0 || y < 0 || x + width > t.w || y + height > t.h) {
            LOG_WARN("updateShaderTexture('%s'): rect %dx%d at (%d,%d) is "
                     "outside the %dx%d texture (ignored)",
                     name.c_str(), width, height, x, y, t.w, t.h);
            return false;
        }
        // Keep the CPU image whole, so a later full rebuild of the GPU copy
        // still carries this write.
        const size_t ch = (size_t)t.channels;
        for (int row = 0; row < height; ++row) {
            const float* src = data + (size_t)row * width * ch;
            float* dst = t.data.data() + (((size_t)(y + row) * t.w) + x) * ch;
            std::copy(src, src + (size_t)width * ch, dst);
        }
        UserTexture::SubUpdate s;
        s.data.assign(data, data + (size_t)width * (size_t)height * ch);
        s.x = x; s.y = y; s.w = width; s.h = height;
        t.subUpdates.push_back(std::move(s));
        bumpChangeGeneration();
        return true;
    }
    LOG_WARN("updateShaderTexture('%s'): no such sampler slot (ignored)",
             name.c_str());
    return false;
}

bool MeshNode::setCustomShaderTextureArray(const std::string& name,
                                           int width, int height, int layers,
                                           int channels, bool mipmap,
                                           bool repeat, bool clampT) {
    const bool release = (width <= 0 || height <= 0 || layers <= 0);
    if (!release && (channels < 1 || channels > 4)) return false;
    for (auto it = userTextures_.begin(); it != userTextures_.end(); ++it) {
        UserTexture& t = *it;
        if (t.name != name) continue;
        if (release) {
            userTextures_.erase(it);
            bumpChangeGeneration();
            return true;
        }
        const bool sameShape = t.layers == layers && t.w == width &&
                               t.h == height && t.channels == channels &&
                               t.mipmap == mipmap && t.repeat == repeat &&
                               t.clampT == clampT;
        if (sameShape) return true;   // storage and written slices survive
        t.data.assign((size_t)width * height * layers * channels, 0.0f);
        t.subUpdates.clear();
        t.sliceUpdates.clear();       // written against the old shape
        t.w = width;
        t.h = height;
        t.layers = layers;
        t.channels = channels;
        t.mipmap = mipmap;
        t.repeat = repeat;
        t.clampT = clampT;
        t.generation = nextResourceGeneration();
        bumpChangeGeneration();
        return true;
    }
    if (release) return true;
    if ((int)userTextures_.size() >= maxUserTextures()) return false;
    UserTexture t;
    t.name = name;
    t.data.assign((size_t)width * height * layers * channels, 0.0f);
    t.w = width;
    t.h = height;
    t.layers = layers;
    t.channels = channels;
    t.mipmap = mipmap;
    t.repeat = repeat;
    t.clampT = clampT;
    t.generation = nextResourceGeneration();
    userTextures_.push_back(std::move(t));
    bumpChangeGeneration();
    return true;
}

bool MeshNode::setCustomShaderTextureArrayLayer(const std::string& name,
                                                int layer, const float* data) {
    for (auto& t : userTextures_) {
        if (t.name != name) continue;
        if (t.layers <= 0 || !data || layer < 0 || layer >= t.layers) {
            LOG_WARN("setShaderTextureArrayLayer('%s', %d): not an array slot "
                     "of that depth, or no data (ignored)", name.c_str(), layer);
            return false;
        }
        const size_t n = (size_t)t.w * (size_t)t.h * (size_t)t.channels;
        std::copy(data, data + n, t.data.data() + (size_t)layer * n);
        // A later write to the same slice supersedes an earlier staged one.
        for (auto& u : t.sliceUpdates) {
            if (u.layer != layer) continue;
            u.data.assign(data, data + n);
            bumpChangeGeneration();
            return true;
        }
        UserTexture::SliceUpdate u;
        u.layer = layer;
        u.data.assign(data, data + n);
        t.sliceUpdates.push_back(std::move(u));
        bumpChangeGeneration();
        return true;
    }
    LOG_WARN("setShaderTextureArrayLayer('%s'): no such sampler slot (ignored)",
             name.c_str());
    return false;
}

void MeshNode::clearCustomShaderTexture(const std::string& name) {
    setCustomShaderTexture(name, 0, 0, nullptr);
}

void MeshNode::setDrawMode(DrawMode m) {
    drawMode_ = m;
    if (m == DrawMode::Lines) {
        // Line meshes have no normals/tangents, so PBR lighting would be
        // garbage. Drop them to the unlit path and out of the shadow pass.
        unlit_ = true;
        castsShadow_ = false;
    }
    bumpChangeGeneration();  // primitive mode changes the rendered silhouette
}

} // namespace bro::scene
