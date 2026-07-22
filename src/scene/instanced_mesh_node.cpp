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

InstancedMeshNode::~InstancedMeshNode() {
    releaseGL();
}

static void ensureTangents(bromesh::MeshData& m) {
    if (m.hasUVs() && m.hasNormals() && !m.hasTangents())
        bromesh::generateTangents(m);
}

void InstancedMeshNode::setMesh(const bromesh::MeshData& mesh) {
    mesh_ = mesh;
    ensureTangents(mesh_);
    meshDirty_ = true;
    batchDirty_ = true;
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void InstancedMeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    meshDirty_ = true;
    batchDirty_ = true;
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // geometry changed — shadow tiles must re-render
}

void InstancedMeshNode::setStaticBatch(bool b) {
    if (staticBatch_ == b) return;
    staticBatch_ = b;
    batchDirty_ = true;
    meshDirty_ = true;       // the uploaded mesh switches between mesh_/batchMesh_
    instancesDirty_ = true;  // and the instance buffer between N rows and 1
    if (!b) batchMesh_.clear();
    bumpChangeGeneration();
}

void InstancedMeshNode::setInstances(const float* data, size_t count) {
    instanceData_.assign(data, data + count * 16);
    instanceCount_ = count;
    instancesDirty_ = true;
    batchDirty_ = true;
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // instance set changed — shadow tiles must re-render
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
    instancesDirty_ = true;
    batchDirty_ = true;
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // instance set changed — shadow tiles must re-render
}

void InstancedMeshNode::updateInstance(size_t i, const float* data16) {
    if (i >= instanceCount_) return;
    std::memcpy(instanceData_.data() + i * 16, data16, sizeof(float) * 16);
    instancesDirty_ = true;
    batchDirty_ = true;
    instanceBoundsDirty_ = true;
    bumpChangeGeneration();  // instance set changed — shadow tiles must re-render
}

void InstancedMeshNode::releaseGL() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ibo_) { glDeleteBuffers(1, &ibo_); ibo_ = 0; }
    if (instVbo_) { glDeleteBuffers(1, &instVbo_); instVbo_ = 0; }
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    if (normalTex_) { glDeleteTextures(1, &normalTex_); normalTex_ = 0; }
    if (mrTex_) { glDeleteTextures(1, &mrTex_); mrTex_ = 0; }
    if (aoTex_) { glDeleteTextures(1, &aoTex_); aoTex_ = 0; }
    if (emissiveTex_) { glDeleteTextures(1, &emissiveTex_); emissiveTex_ = 0; }
    indexCount_ = 0;
    instVboCapacity_ = 0;
}

static void stage(InstancedMeshNode::PendingTex& p, int w, int h, const uint8_t* rgba) {
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

void InstancedMeshNode::setBaseColorTexture(int w, int h, const uint8_t* rgba) { stage(pendingBase_, w, h, rgba); }
void InstancedMeshNode::clearBaseColorTexture() { stage(pendingBase_, 0, 0, nullptr); }
void InstancedMeshNode::setNormalTexture(int w, int h, const uint8_t* rgba) { stage(pendingNormal_, w, h, rgba); }
void InstancedMeshNode::clearNormalTexture() { stage(pendingNormal_, 0, 0, nullptr); }
void InstancedMeshNode::setMetallicRoughnessTexture(int w, int h, const uint8_t* rgba) { stage(pendingMR_, w, h, rgba); }
void InstancedMeshNode::clearMetallicRoughnessTexture() { stage(pendingMR_, 0, 0, nullptr); }
void InstancedMeshNode::setOcclusionTexture(int w, int h, const uint8_t* rgba) { stage(pendingAO_, w, h, rgba); }
void InstancedMeshNode::clearOcclusionTexture() { stage(pendingAO_, 0, 0, nullptr); }
void InstancedMeshNode::setEmissiveTexture(int w, int h, const uint8_t* rgba) { stage(pendingEmissive_, w, h, rgba); }
void InstancedMeshNode::clearEmissiveTexture() { stage(pendingEmissive_, 0, 0, nullptr); }

// Upload mipmaps that preserve alpha-tested coverage. Without this, a
// sparse alpha-cutout atlas (foliage cards, sprites, decals) loses its
// thresholded silhouette as LOD increases — the box-filter average of
// many transparent pixels falls below the cutoff and the cutout vanishes
// entirely. Castano's technique: pick a per-level alpha scale so each
// mip's coverage at cutoff 0.5 matches level 0's. For fully-opaque
// textures this collapses to scale = 1, so it's a safe default for all
// RGBA inputs.
static void uploadAlphaCoverageMipmaps(int w0, int h0, const uint8_t* base) {
    constexpr float kCutoff = 0.5f * 255.0f;
    auto coverage = [&](const std::vector<uint8_t>& lvl, int w, int h, float scale) {
        size_t over = 0;
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; ++i) {
            float a = (float)lvl[i * 4 + 3] * scale;
            if (a >= kCutoff) ++over;
        }
        return (float)over / (float)n;
    };

    std::vector<uint8_t> lvl(base, base + (size_t)w0 * (size_t)h0 * 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w0, h0, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, lvl.data());
    const float baseCov = coverage(lvl, w0, h0, 1.0f);

    int w = w0, h = h0, mip = 0;
    std::vector<uint8_t> next, scaled;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        next.resize((size_t)nw * (size_t)nh * 4);
        for (int y = 0; y < nh; ++y) {
            const int sy0 = std::min(y * 2, h - 1);
            const int sy1 = std::min(y * 2 + 1, h - 1);
            for (int x = 0; x < nw; ++x) {
                const int sx0 = std::min(x * 2, w - 1);
                const int sx1 = std::min(x * 2 + 1, w - 1);
                for (int c = 0; c < 4; ++c) {
                    int s = lvl[(sy0 * w + sx0) * 4 + c]
                          + lvl[(sy0 * w + sx1) * 4 + c]
                          + lvl[(sy1 * w + sx0) * 4 + c]
                          + lvl[(sy1 * w + sx1) * 4 + c];
                    next[(y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                }
            }
        }
        // Binary-search scale that matches base coverage (only meaningful
        // when 0 < baseCov < 1; otherwise scale stays at 1).
        float scale = 1.0f;
        if (baseCov > 0.0f && baseCov < 1.0f) {
            // Upper bound generous enough for sparse atlases where deep
            // mips average alpha well below the cutoff (4 was too tight
            // for foliage cards — small leaves washed out at distance).
            float lo = 0.0f, hi = 64.0f;
            for (int it = 0; it < 18; ++it) {
                const float mid = (lo + hi) * 0.5f;
                if (coverage(next, nw, nh, mid) > baseCov) hi = mid;
                else lo = mid;
            }
            scale = (lo + hi) * 0.5f;
        }
        scaled = next;
        if (scale != 1.0f) {
            const size_t n = (size_t)nw * (size_t)nh;
            for (size_t i = 0; i < n; ++i) {
                float a = (float)scaled[i * 4 + 3] * scale;
                if (a > 255.0f) a = 255.0f;
                scaled[i * 4 + 3] = (uint8_t)a;
            }
        }
        ++mip;
        glTexImage2D(GL_TEXTURE_2D, mip, GL_RGBA8, nw, nh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, scaled.data());
        // Continue downsampling from the un-scaled chain so each level's
        // alpha derives from the original base rather than compounding.
        lvl = std::move(next);
        w = nw; h = nh;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mip);
}

static void flushTex(InstancedMeshNode::PendingTex& p, GLuint& glTex) {
    if (!p.dirty) return;
    if (p.w > 0 && p.h > 0 && !p.data.empty()) {
        if (!glTex) glGenTextures(1, &glTex);
        glBindTexture(GL_TEXTURE_2D, glTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        uploadAlphaCoverageMipmaps(p.w, p.h, p.data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        p.data.clear();
        p.data.shrink_to_fit();
    } else if (glTex) {
        glDeleteTextures(1, &glTex);
        glTex = 0;
    }
    p.dirty = false;
}

// Bake mesh_ + instanceData_ into batchMesh_: one copy of the mesh per
// instance, transformed into node space, with the instance RGB tint folded
// into vertex colours and the atlas cell folded into UVs, all merged. The
// draw then renders batchMesh_ as a single identity instance (see
// setStaticBatch). O(total verts); only runs when batchDirty_ && renderingBatched.
void InstancedMeshNode::rebuildStaticBatch() {
    batchDirty_ = false;
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
    meshDirty_ = true;       // batchMesh_ must be (re)uploaded
    instancesDirty_ = true;  // single identity instance row
}

void InstancedMeshNode::uploadMeshToGPU() {
    // Static batch uploads the merged geometry; otherwise the authored mesh.
    const bromesh::MeshData& M = renderingBatched() ? batchMesh_ : mesh_;
    if (M.empty()) return;

    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);
    if (!ibo_) glGenBuffers(1, &ibo_);

    glBindVertexArray(vao_);

    size_t vertCount = M.vertexCount();
    bool hasNormals = M.hasNormals();
    bool hasUVs = M.hasUVs();
    bool hasColors = M.hasColors();
    bool hasTangents = M.hasTangents();
    hasVertexColors_ = hasColors;

    size_t stride = 3;
    if (hasNormals) stride += 3;
    if (hasUVs) stride += 2;
    if (hasColors) stride += 4;
    if (hasTangents) stride += 4;

    std::vector<float> interleaved(vertCount * stride);
    for (size_t i = 0; i < vertCount; i++) {
        size_t off = i * stride;
        interleaved[off + 0] = M.positions[i * 3 + 0];
        interleaved[off + 1] = M.positions[i * 3 + 1];
        interleaved[off + 2] = M.positions[i * 3 + 2];
        size_t at = 3;
        if (hasNormals) {
            interleaved[off + at + 0] = M.normals[i * 3 + 0];
            interleaved[off + at + 1] = M.normals[i * 3 + 1];
            interleaved[off + at + 2] = M.normals[i * 3 + 2];
            at += 3;
        }
        if (hasUVs) {
            interleaved[off + at + 0] = M.uvs[i * 2 + 0];
            interleaved[off + at + 1] = M.uvs[i * 2 + 1];
            at += 2;
        }
        if (hasColors) {
            interleaved[off + at + 0] = M.colors[i * 4 + 0];
            interleaved[off + at + 1] = M.colors[i * 4 + 1];
            interleaved[off + at + 2] = M.colors[i * 4 + 2];
            interleaved[off + at + 3] = M.colors[i * 4 + 3];
            at += 4;
        }
        if (hasTangents) {
            interleaved[off + at + 0] = M.tangents[i * 4 + 0];
            interleaved[off + at + 1] = M.tangents[i * 4 + 1];
            interleaved[off + at + 2] = M.tangents[i * 4 + 2];
            interleaved[off + at + 3] = M.tangents[i * 4 + 3];
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 interleaved.size() * sizeof(float),
                 interleaved.data(), GL_STATIC_DRAW);

    GLsizei byteStride = (GLsizei)(stride * sizeof(float));

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, byteStride, (void*)0);

    if (hasNormals) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, byteStride, (void*)(3 * sizeof(float)));
    } else {
        glDisableVertexAttribArray(1);
    }
    if (hasUVs) {
        size_t uvOffset = 3 + (hasNormals ? 3 : 0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, byteStride, (void*)(uvOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(2);
    }
    if (hasColors) {
        size_t colorOffset = 3 + (hasNormals ? 3 : 0) + (hasUVs ? 2 : 0);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, byteStride, (void*)(colorOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(3);
    }
    if (hasTangents) {
        size_t tanOffset = 3 + (hasNormals ? 3 : 0) + (hasUVs ? 2 : 0) + (hasColors ? 4 : 0);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, byteStride, (void*)(tanOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(4);
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 M.indices.size() * sizeof(uint32_t),
                 M.indices.data(), GL_STATIC_DRAW);
    indexCount_ = (GLsizei)M.indices.size();

    // Re-bind the instance buffer's attributes (locations 8..11) into this
    // VAO. The instance VBO itself may not be uploaded yet; the bindings are
    // valid even with an empty buffer and become live once we glBufferData
    // into instVbo_ during uploadInstancesToGPU().
    if (!instVbo_) glGenBuffers(1, &instVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
    GLsizei instStride = (GLsizei)(16 * sizeof(float));
    for (int loc = 8; loc <= 11; ++loc) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, instStride,
                              (void*)((loc - 8) * 4 * sizeof(float)));
        glVertexAttribDivisor(loc, 1);
    }

    glBindVertexArray(0);
    meshDirty_ = false;

    flushTex(pendingBase_,     texture_);
    flushTex(pendingNormal_,   normalTex_);
    flushTex(pendingMR_,       mrTex_);
    flushTex(pendingAO_,       aoTex_);
    flushTex(pendingEmissive_, emissiveTex_);
}

void InstancedMeshNode::uploadInstancesToGPU() {
    // A batched draw is one identity instance (white tint — per-instance colour
    // is already baked into the merged mesh's vertex colours).
    static const float kIdentityInst[16] = {
        1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0,   1, 1, 1, 1,
    };
    const bool batched = renderingBatched();
    const float* src = batched ? kIdentityInst : instanceData_.data();
    const size_t floats = batched ? 16 : instanceData_.size();

    if (!instVbo_) glGenBuffers(1, &instVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
    size_t bytes = floats * sizeof(float);
    if (bytes > instVboCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, src, GL_DYNAMIC_DRAW);
        instVboCapacity_ = bytes;
    } else if (bytes > 0) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, src);
    }
    instancesDirty_ = false;
}

void InstancedMeshNode::onRender(SceneGraph& graph) {
    (void)graph;
    drawRawInstanced();
}

bool InstancedMeshNode::drawRawInstanced() {
    if (mesh_.empty() || instanceCount_ == 0) return false;
    if (renderingBatched() && batchDirty_) rebuildStaticBatch();
    if (meshDirty_) uploadMeshToGPU();
    if (instancesDirty_) uploadInstancesToGPU();
    if (!vao_ || indexCount_ == 0) return false;

    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr,
                            (GLsizei)(renderingBatched() ? 1 : instanceCount_));
    glBindVertexArray(0);
    return true;
}

bool InstancedMeshNode::computeWorldInstanceBounds(float outMin[3], float outMax[3]) const {
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
    // actually renders (renderInstancedMeshNode applies the same
    // worldMatrix()). Transforming the cached box is slightly looser than
    // transforming every instance corner, but stays conservative.
    bromath::AABB3 wb = bromath::atransform(instanceBoundsCache_, worldMatrix());
    outMin[0] = wb.min.x; outMin[1] = wb.min.y; outMin[2] = wb.min.z;
    outMax[0] = wb.max.x; outMax[1] = wb.max.y; outMax[2] = wb.max.z;
    return true;
}

bool InstancedMeshNode::drawRawInstancedDepth() {
    // Same VAO as the forward pass — the depth-only shader reads aPos
    // (location 0) and the per-instance matrix attributes (8..10). Other
    // vertex attributes are simply unused.
    return drawRawInstanced();
}

} // namespace bro::scene
