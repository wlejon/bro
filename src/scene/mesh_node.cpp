#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/gpu_upload_stats.h"
#include "util/log.h"

#include <bromesh/analysis/bbox.h>
#include <bromesh/manipulation/normals.h>

#include <algorithm>
#include <cstring>

namespace bro::scene {

int MeshNode::userTextureUnitLimit() {
    return 32;
}

int MeshNode::maxUserTextures() {
    const int n = userTextureUnitLimit() - kUserTextureUnitBase;
    return n > 0 ? n : 0;
}

MeshNode::MeshNode(const std::string& name) : SceneNode(name) {}

MeshNode::~MeshNode() {
    releaseGL();
}

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
    gpuDirty_ = true;
    bvhDirty_ = true;
    recomputeBounds();
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void MeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    gpuDirty_ = true;
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
    // Stage the old chain's GL names for deletion on the GL thread.
    for (auto& e : lods_) {
        if (e.vao) deadLodVaos_.push_back(e.vao);
        if (e.vbo) deadLodBufs_.push_back(e.vbo);
        if (e.ibo) deadLodBufs_.push_back(e.ibo);
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
    if (!lods_.empty()) hasVertexColors_ = lods_[0].hasColors;
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

void MeshNode::flushDeadLodBuffers() {
    deadLodVaos_.clear();
    deadLodBufs_.clear();
}

const bromesh::MeshBVH& MeshNode::bvh() const {
    if (bvhDirty_) {
        bvh_ = bromesh::MeshBVH::build(mesh_);
        bvhDirty_ = false;
    }
    return bvh_;
}

void MeshNode::releaseGL() {
    vao_ = 0;
    vbo_ = 0;
    ibo_ = 0;
    for (auto& e : lods_) {
        e.vao = 0;
        e.vbo = 0;
        e.ibo = 0;
        e.indexCount = 0;
        e.gpuDirty = true;
    }
    flushDeadLodBuffers();
    texture_ = 0;
    normalTex_ = 0;
    mrTex_ = 0;
    aoTex_ = 0;
    emissiveTex_ = 0;
    for (auto& t : userTextures_) {
        t.tex = 0;
    }
    userTextures_.clear();
    indexCount_ = 0;
}

static void stage(MeshNode::PendingTex& p, int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) {
        p.data.clear();
        p.w = 0;
        p.h = 0;
    } else {
        p.data.assign(rgba, rgba + (size_t)w * (size_t)h * 4);
        p.w = w;
        p.h = h;
    }
    p.dirty = true;
}

void MeshNode::setBaseColorTexture(int width, int height, const uint8_t* rgba) {
    externalBaseColorTex_ = nullptr;  // owned bytes win; drop the live link
    externalSceneProvider_ = nullptr;
    stage(pendingBase_, width, height, rgba);
}
void MeshNode::clearBaseColorTexture() {
    externalBaseColorTex_ = nullptr;
    externalSceneProvider_ = nullptr;
    stage(pendingBase_, 0, 0, nullptr);
}

void MeshNode::setExternalBaseColorTexture(ExternalTextureProvider provider) {
    externalBaseColorTex_ = std::move(provider);
    // Stage a clear of the owned slot so a previously uploaded texture is
    // deleted at the next flush — setters may run without a GL context
    // current, so the delete cannot happen here.
    stage(pendingBase_, 0, 0, nullptr);
}

void MeshNode::setNormalTexture(int width, int height, const uint8_t* rgba) {
    stage(pendingNormal_, width, height, rgba);
}
void MeshNode::clearNormalTexture() { stage(pendingNormal_, 0, 0, nullptr); }

void MeshNode::setMetallicRoughnessTexture(int width, int height, const uint8_t* rgba) {
    stage(pendingMR_, width, height, rgba);
}
void MeshNode::clearMetallicRoughnessTexture() { stage(pendingMR_, 0, 0, nullptr); }

void MeshNode::setOcclusionTexture(int width, int height, const uint8_t* rgba) {
    stage(pendingAO_, width, height, rgba);
}
void MeshNode::clearOcclusionTexture() { stage(pendingAO_, 0, 0, nullptr); }

void MeshNode::setEmissiveTexture(int width, int height, const uint8_t* rgba) {
    stage(pendingEmissive_, width, height, rgba);
}
void MeshNode::clearEmissiveTexture() { stage(pendingEmissive_, 0, 0, nullptr); }

static void flushTex(MeshNode::PendingTex& p, GLuint& glTex) {
    (void)glTex;
    p.dirty = false;
}

bool MeshNode::setCustomShaderTexture(const std::string& name, int width,
                                      int height, const float* data,
                                      bool mipmap, bool repeat,
                                      bool clampT, int channels) {
    const bool release = (width <= 0 || height <= 0 || !data);
    for (auto& t : userTextures_) {
        if (t.name != name) continue;
        // A full upload replaces the whole image, so any sub-rect writes
        // staged against the OLD contents are superseded — dropping them here
        // is what keeps the flush order (full first, then subs) equivalent to
        // the order the setters were called in.
        t.subUpdates.clear();
        // A 2D upload onto what was an array slot turns it back into a plain
        // 2D slot; the flush sees the target change and recreates the name.
        t.sliceUpdates.clear();
        t.layers = 0;
        if (release) {
            t.data.clear();
            t.data.shrink_to_fit();
            t.w = t.h = 0;
            t.channels = 1;
        } else {
            t.data.assign(data, data + (size_t)width * (size_t)height * (size_t)channels);
            t.w = width;
            t.h = height;
            t.channels = channels;
            t.mipmap = mipmap;
            t.repeat = repeat;
            t.clampT = clampT;
        }
        t.dirty = true;
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
    t.dirty = true;
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
        // Bound against the staged extent, not the GL texture: the slot may
        // not have flushed yet, and t.w/t.h are the dimensions it WILL have.
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
        UserTexture::SubUpdate s;
        s.data.assign(data, data + (size_t)width * (size_t)height * (size_t)t.channels);
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
    for (auto& t : userTextures_) {
        if (t.name != name) continue;
        if (release) {
            t.data.clear();
            t.data.shrink_to_fit();
            t.subUpdates.clear();
            t.sliceUpdates.clear();
            t.w = t.h = 0;
            t.layers = 0;
            t.channels = 1;
            t.dirty = true;
            bumpChangeGeneration();
            return true;
        }
        const bool sameShape = t.layers == layers && t.w == width &&
                               t.h == height && t.channels == channels &&
                               t.mipmap == mipmap && t.repeat == repeat &&
                               t.clampT == clampT;
        if (sameShape) return true;   // storage and written slices survive
        t.data.clear();
        t.data.shrink_to_fit();
        t.subUpdates.clear();
        t.sliceUpdates.clear();       // written against the old shape
        t.w = width;
        t.h = height;
        t.layers = layers;
        t.channels = channels;
        t.mipmap = mipmap;
        t.repeat = repeat;
        t.clampT = clampT;
        t.dirty = true;
        bumpChangeGeneration();
        return true;
    }
    if (release) return true;
    if ((int)userTextures_.size() >= maxUserTextures()) return false;
    UserTexture t;
    t.name = name;
    t.w = width;
    t.h = height;
    t.layers = layers;
    t.channels = channels;
    t.mipmap = mipmap;
    t.repeat = repeat;
    t.clampT = clampT;
    t.dirty = true;
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

static void flushUserTexArray(MeshNode::UserTexture& t) {
    t.sliceUpdates.clear();
    t.dirty = false;
}

static void flushUserTex(MeshNode::UserTexture& t) {
    t.subUpdates.clear();
    t.dirty = false;
}

void MeshNode::flushPendingTexturesImpl() {
    flushTex(pendingBase_,     texture_);
    flushTex(pendingNormal_,   normalTex_);
    flushTex(pendingMR_,       mrTex_);
    flushTex(pendingAO_,       aoTex_);
    flushTex(pendingEmissive_, emissiveTex_);
    for (auto& t : userTextures_) flushUserTex(t);
    // Drop fully-released slots so the name can be re-bound later without
    // counting against the unit budget.
    userTextures_.erase(
        std::remove_if(userTextures_.begin(), userTextures_.end(),
                       [](const UserTexture& t) {
                           return t.tex == 0 && !t.dirty && t.w == 0;
                       }),
        userTextures_.end());
}

static void uploadInterleavedMesh(const bromesh::MeshData& mesh,
                                  GLuint& /*vao*/, GLuint& /*vbo*/, GLuint& /*ibo*/,
                                  GLsizei& indexCount) {
    indexCount = (GLsizei)mesh.indices.size();
}

const float* MeshNode::normalMatrix3(const bromath::Mat4& model) const {
    float src[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            src[c * 3 + r] = model.at(r, c);

    if (normalMatValid_ && std::memcmp(src, normalMatSrc_, sizeof(src)) == 0)
        return normalMat3_;

    // minverse returns identity for a singular matrix (zero scale), so
    // degenerate nodes fall back to untransformed normals instead of NaNs.
    const bromath::Mat4 invT = bromath::mtranspose(bromath::minverse(model));
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            normalMat3_[c * 3 + r] = invT.at(r, c);

    std::memcpy(normalMatSrc_, src, sizeof(src));
    normalMatValid_ = true;
    return normalMat3_;
}

void MeshNode::uploadToGPU() {
    if (mesh_.empty()) return;
    hasVertexColors_ = mesh_.hasColors();
    indexCount_ = (GLsizei)mesh_.indices.size();
    gpuDirty_ = false;
    flushPendingTextures();
}

void MeshNode::onRender(SceneGraph& /*graph*/) {
}

bool MeshNode::drawRaw() {
    return false;
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
