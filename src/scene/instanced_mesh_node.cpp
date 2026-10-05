#include "scene/instanced_mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/merge.h>
#include <bromesh/manipulation/transform.h>
#include <bromesh/analysis/bbox.h>

#include <cmath>
#include <cstring>

namespace bro::scene {

InstancedMeshNode::InstancedMeshNode(const std::string& name) : SceneNode(name) {}


static void ensureTangents(bromesh::MeshData& m) {
    if (m.hasUVs() && m.hasNormals() && !m.hasTangents())
        bromesh::generateTangents(m);
}

void InstancedMeshNode::setMesh(const bromesh::MeshData& mesh) {
    mesh_ = mesh;
    ensureTangents(mesh_);
    hasVertexColors_ = mesh_.hasColors();
    geometryGeneration_ = nextResourceGeneration();
    batchDirty_ = true;
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
    instanceBoundsDirty_ = true;
    bvhDirty_ = true;
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void InstancedMeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    hasVertexColors_ = mesh_.hasColors();
    geometryGeneration_ = nextResourceGeneration();
    batchDirty_ = true;
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
    instanceBoundsDirty_ = true;
    bvhDirty_ = true;
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

const bromesh::MeshBVH& InstancedMeshNode::bvh() const {
    if (bvhDirty_) {
        bvh_ = bromesh::MeshBVH::build(mesh_);
        bvhDirty_ = false;
    }
    return bvh_;
}

void InstancedMeshNode::setStaticBatch(bool b) {
    if (staticBatch_ == b) return;
    staticBatch_ = b;
    batchDirty_ = true;
    if (!b) batchMesh_.clear();
    bumpChangeGeneration();
}

void InstancedMeshNode::instancesChanged() {
    batchDirty_ = true;
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // instance set changed — shadow tiles must re-render
}

void InstancedMeshNode::setInstances(const float* data, size_t count) {
    instanceData_.assign(data, data + count * 16);
    instanceCount_ = count;
    instancesChanged();
}

void InstancedMeshNode::setInstancesFromPosQuatScale(const float* data, size_t count) {
    instanceData_.resize(count * 16);
    instanceCount_ = count;
    for (size_t i = 0; i < count; ++i) {
        const float* in = data + i * 9;
        float px = in[0], py = in[1], pz = in[2];
        float qx = in[3], qy = in[4], qz = in[5], qw = in[6];
        float s  = in[7];
        float variantIdx = in[8];

        // Quaternion to 3x3 rotation, then multiply by uniform scale.
        float xx = qx * qx, yy = qy * qy, zz = qz * qz;
        float xy = qx * qy, xz = qx * qz, yz = qy * qz;
        float wx = qw * qx, wy = qw * qy, wz = qw * qz;

        float r00 = (1.0f - 2.0f * (yy + zz)) * s;
        float r01 = (2.0f * (xy - wz))        * s;
        float r02 = (2.0f * (xz + wy))        * s;
        float r10 = (2.0f * (xy + wz))        * s;
        float r11 = (1.0f - 2.0f * (xx + zz)) * s;
        float r12 = (2.0f * (yz - wx))        * s;
        float r20 = (2.0f * (xz - wy))        * s;
        float r21 = (2.0f * (yz + wx))        * s;
        float r22 = (1.0f - 2.0f * (xx + yy)) * s;

        float* o = instanceData_.data() + i * 16;
        o[ 0] = r00; o[ 1] = r01; o[ 2] = r02; o[ 3] = px;
        o[ 4] = r10; o[ 5] = r11; o[ 6] = r12; o[ 7] = py;
        o[ 8] = r20; o[ 9] = r21; o[10] = r22; o[11] = pz;
        o[12] = 1.0f; o[13] = 1.0f; o[14] = 1.0f;
        // Pack variantIndex into alpha as (idx + 0.5) / 256, so the shader
        // can recover idx = int(a * 256). Clamp to [0, 255].
        float idxClamped = variantIdx < 0.0f ? 0.0f : (variantIdx > 255.0f ? 255.0f : variantIdx);
        o[15] = (idxClamped + 0.5f) / 256.0f;
    }
    instancesChanged();
}

void InstancedMeshNode::updateInstance(size_t i, const float* data16) {
    if (i >= instanceCount_) return;
    std::memcpy(instanceData_.data() + i * 16, data16, sizeof(float) * 16);
    instancesChanged();
}

void InstancedMeshNode::setScatterSegments(const float* segData, size_t segCount,
                                           const float* instSeg, size_t instCount,
                                           const ScatterParams& params,
                                           const float boundsMin[3],
                                           const float boundsMax[3]) {
    scatterMode_ = true;
    scatterData_.assign(segData, segData + segCount * 8);
    scatterInstSeg_.assign(instSeg, instSeg + instCount);
    scatterSegCount_ = segCount;
    scatterInstCount_ = instCount;
    scatterParams_ = params;
    scatterBounds_.min = {boundsMin[0], boundsMin[1], boundsMin[2]};
    scatterBounds_.max = {boundsMax[0], boundsMax[1], boundsMax[2]};
    bumpChangeGeneration();
}

void InstancedMeshNode::setTubeSegments(const float* segData, size_t segCount,
                                        int sides, float radiusScale,
                                        const float boundsMin[3],
                                        const float boundsMax[3]) {
    tubeMode_ = true;
    tubeData_.assign(segData, segData + segCount * 8);
    tubeSegCount_ = segCount;
    tubeSides_ = sides < 3 ? 3 : sides;
    tubeRadiusScale_ = radiusScale;
    tubeBounds_.min = {boundsMin[0], boundsMin[1], boundsMin[2]};
    tubeBounds_.max = {boundsMax[0], boundsMax[1], boundsMax[2]};
    bumpChangeGeneration();
}

void InstancedMeshNode::setBaseColorTexture(int w, int h, const uint8_t* rgba) { baseColorTex_.set(w, h, rgba); }
void InstancedMeshNode::clearBaseColorTexture() { baseColorTex_.clear(); }
void InstancedMeshNode::setNormalTexture(int w, int h, const uint8_t* rgba) { normalTex_.set(w, h, rgba); }
void InstancedMeshNode::clearNormalTexture() { normalTex_.clear(); }
void InstancedMeshNode::setMetallicRoughnessTexture(int w, int h, const uint8_t* rgba) { mrTex_.set(w, h, rgba); }
void InstancedMeshNode::clearMetallicRoughnessTexture() { mrTex_.clear(); }
void InstancedMeshNode::setOcclusionTexture(int w, int h, const uint8_t* rgba) { aoTex_.set(w, h, rgba); }
void InstancedMeshNode::clearOcclusionTexture() { aoTex_.clear(); }
void InstancedMeshNode::setEmissiveTexture(int w, int h, const uint8_t* rgba) { emissiveTex_.set(w, h, rgba); }
void InstancedMeshNode::clearEmissiveTexture() { emissiveTex_.clear(); }

// Bake mesh_ + instanceData_ into batchMesh_: one copy of the mesh per
// instance, transformed into node space, with the instance RGB tint folded
// into vertex colours and the atlas cell folded into UVs, all merged. The
// draw then renders batchMesh_ as a single identity instance (see
// setStaticBatch). O(total verts); only runs when batchDirty_ && renderingBatched.
void InstancedMeshNode::rebuildStaticBatch() const {
    batchDirty_ = false;
    batchGeneration_ = nextResourceGeneration();
    batchMesh_.clear();
    if (mesh_.empty() || instanceCount_ == 0) return;

    const int cols = atlasCols_ < 1 ? 1 : atlasCols_;
    const int rows = atlasRows_ < 1 ? 1 : atlasRows_;
    const bool atlas = (cols > 1 || rows > 1) && mesh_.hasUVs();

    std::vector<bromesh::MeshData> parts;
    parts.reserve(instanceCount_);
    for (size_t i = 0; i < instanceCount_; ++i) {
        const float* r = instanceData_.data() + i * 16;
        // Instance rows are a 4x3 ROW-major affine (rows r0..r2, translation in
        // .w). transformMesh wants a COLUMN-major 4x4 — transpose the 3x3 and
        // put translation in the last column.
        const float m[16] = {
            r[0], r[4], r[8],  0.0f,   // col 0
            r[1], r[5], r[9],  0.0f,   // col 1
            r[2], r[6], r[10], 0.0f,   // col 2
            r[3], r[7], r[11], 1.0f,   // col 3 (translation)
        };
        bromesh::MeshData part = mesh_;
        bromesh::transformMesh(part, m);

        const size_t vc = part.vertexCount();
        // Fold per-instance RGB tint (instance row .w column, indices 12..14)
        // into vertex colours so the single merged instance keeps per-tree tint.
        const float tr = r[12], tg = r[13], tb = r[14];
        if (part.colors.size() != vc * 4) part.colors.assign(vc * 4, 1.0f);
        for (size_t v = 0; v < vc; ++v) {
            part.colors[v * 4 + 0] *= tr;
            part.colors[v * 4 + 1] *= tg;
            part.colors[v * 4 + 2] *= tb;
        }
        // Fold the atlas cell (packed in r[15] as (idx+0.5)/256) into UVs,
        // matching the shader's remap: uv = (cell.xy + fract(uv)) * cellSize.
        if (atlas) {
            int cell = (int)(r[15] * 256.0f);
            const int total = cols * rows;
            if (cell < 0) cell = 0;
            if (cell >= total) cell = total - 1;
            const int cx = cell % cols, cy = cell / cols;
            const float sw = 1.0f / (float)cols, sh = 1.0f / (float)rows;
            for (size_t v = 0; v < vc; ++v) {
                float u = part.uvs[v * 2 + 0], w = part.uvs[v * 2 + 1];
                u -= std::floor(u); w -= std::floor(w);
                part.uvs[v * 2 + 0] = ((float)cx + u) * sw;
                part.uvs[v * 2 + 1] = ((float)cy + w) * sh;
            }
        }
        parts.push_back(std::move(part));
    }
    batchMesh_ = bromesh::mergeMeshes(parts);
    ensureTangents(batchMesh_);
}

bool InstancedMeshNode::computeWorldInstanceBounds(float outMin[3], float outMax[3]) const {
    if (scatterMode_) {
        if (scatterSegCount_ == 0) return false;
        // Explicit node-local bounds supplied at setScatterSegments (segment
        // AABB padded for leaf reach); fold in the parent-chain transform.
        bromath::AABB3 wb = bromath::atransform(scatterBounds_, worldMatrix());
        outMin[0] = wb.min.x; outMin[1] = wb.min.y; outMin[2] = wb.min.z;
        outMax[0] = wb.max.x; outMax[1] = wb.max.y; outMax[2] = wb.max.z;
        return true;
    }
    if (tubeMode_) {
        if (tubeSegCount_ == 0) return false;
        bromath::AABB3 wb = bromath::atransform(tubeBounds_, worldMatrix());
        outMin[0] = wb.min.x; outMin[1] = wb.min.y; outMin[2] = wb.min.z;
        outMax[0] = wb.max.x; outMax[1] = wb.max.y; outMax[2] = wb.max.z;
        return true;
    }
    if (mesh_.empty() || instanceCount_ == 0) return false;

    // Node-space union of all instance-transformed mesh bounds. O(instances),
    // so it's cached and only rebuilt when the mesh or instance buffer
    // changes — frustum culling queries this every frame.
    if (instanceBoundsDirty_) {
        const float lx0 = bounds_.min.x, ly0 = bounds_.min.y, lz0 = bounds_.min.z;
        const float lx1 = bounds_.max.x, ly1 = bounds_.max.y, lz1 = bounds_.max.z;
        bromath::AABB3 nb = bromath::aempty3();
        for (size_t i = 0; i < instanceCount_; ++i) {
            const float* o = instanceData_.data() + i * 16;
            // Row-major 4x3: o[0..3] = row0 (m00 m01 m02 tx), etc.
            for (int c = 0; c < 8; ++c) {
                float lp[3] = {
                    (c & 1) ? lx1 : lx0,
                    (c & 2) ? ly1 : ly0,
                    (c & 4) ? lz1 : lz0,
                };
                nb = bromath::aexpand(nb, bromath::Vec3{
                    o[ 0] * lp[0] + o[ 1] * lp[1] + o[ 2] * lp[2] + o[ 3],
                    o[ 4] * lp[0] + o[ 5] * lp[1] + o[ 6] * lp[2] + o[ 7],
                    o[ 8] * lp[0] + o[ 9] * lp[1] + o[10] * lp[2] + o[11]});
            }
        }
        instanceBoundsCache_ = nb;
        instanceBoundsDirty_ = false;
    }

    // Fold in the node's own parent-chain transform so this matches what
    // actually renders (the draw applies the same worldMatrix()). Transforming the cached box is slightly looser than
    // transforming every instance corner, but stays conservative.
    bromath::AABB3 wb = bromath::atransform(instanceBoundsCache_, worldMatrix());
    outMin[0] = wb.min.x; outMin[1] = wb.min.y; outMin[2] = wb.min.z;
    outMax[0] = wb.max.x; outMax[1] = wb.max.y; outMax[2] = wb.max.z;
    return true;
}

} // namespace bro::scene
