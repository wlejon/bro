#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/analysis/bbox.h>
#include <bromesh/manipulation/normals.h>

#include <algorithm>

namespace bro::scene {

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
    if (!deadLodVaos_.empty()) {
        glDeleteVertexArrays((GLsizei)deadLodVaos_.size(), deadLodVaos_.data());
        deadLodVaos_.clear();
    }
    if (!deadLodBufs_.empty()) {
        glDeleteBuffers((GLsizei)deadLodBufs_.size(), deadLodBufs_.data());
        deadLodBufs_.clear();
    }
}

const bromesh::MeshBVH& MeshNode::bvh() const {
    if (bvhDirty_) {
        bvh_ = bromesh::MeshBVH::build(mesh_);
        bvhDirty_ = false;
    }
    return bvh_;
}

void MeshNode::releaseGL() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ibo_) { glDeleteBuffers(1, &ibo_); ibo_ = 0; }
    for (auto& e : lods_) {
        if (e.vao) { glDeleteVertexArrays(1, &e.vao); e.vao = 0; }
        if (e.vbo) { glDeleteBuffers(1, &e.vbo); e.vbo = 0; }
        if (e.ibo) { glDeleteBuffers(1, &e.ibo); e.ibo = 0; }
        e.indexCount = 0;
        e.gpuDirty = true;
    }
    flushDeadLodBuffers();
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    if (normalTex_) { glDeleteTextures(1, &normalTex_); normalTex_ = 0; }
    if (mrTex_) { glDeleteTextures(1, &mrTex_); mrTex_ = 0; }
    if (aoTex_) { glDeleteTextures(1, &aoTex_); aoTex_ = 0; }
    if (emissiveTex_) { glDeleteTextures(1, &emissiveTex_); emissiveTex_ = 0; }
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
    stage(pendingBase_, width, height, rgba);
}
void MeshNode::clearBaseColorTexture() {
    externalBaseColorTex_ = nullptr;
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

// Upload or release a staged texture slot. Consumes p.dirty; frees staged CPU
// bytes after upload. glTex must be the owning GL name (zeroed when released).
static void flushTex(MeshNode::PendingTex& p, GLuint& glTex) {
    if (!p.dirty) return;
    if (p.w > 0 && p.h > 0 && !p.data.empty()) {
        if (!glTex) glGenTextures(1, &glTex);
        glBindTexture(GL_TEXTURE_2D, glTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     p.w, p.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, p.data.data());
        glGenerateMipmap(GL_TEXTURE_2D);
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

void MeshNode::flushPendingTextures() {
    flushTex(pendingBase_,     texture_);
    flushTex(pendingNormal_,   normalTex_);
    flushTex(pendingMR_,       mrTex_);
    flushTex(pendingAO_,       aoTex_);
    flushTex(pendingEmissive_, emissiveTex_);
}

// Interleave a MeshData's attribute streams and upload them into the given
// buffer set (creating GL names on first use). Shared by the base-mesh
// upload (uploadToGPU) and per-LOD-level uploads; GL thread only.
static void uploadInterleavedMesh(const bromesh::MeshData& mesh,
                                  GLuint& vao, GLuint& vbo, GLuint& ibo,
                                  GLsizei& indexCount) {
    // Create GL objects if needed
    if (!vao) glGenVertexArrays(1, &vao);
    if (!vbo) glGenBuffers(1, &vbo);
    if (!ibo) glGenBuffers(1, &ibo);

    glBindVertexArray(vao);

    // Interleave: pos(3) + normal(3) + uv(2) + color(4) + tangent(4)
    size_t vertCount = mesh.vertexCount();
    bool hasNormals = mesh.hasNormals();
    bool hasUVs = mesh.hasUVs();
    bool hasColors = mesh.hasColors();
    bool hasTangents = mesh.hasTangents();

    size_t stride = 3; // position always
    if (hasNormals) stride += 3;
    if (hasUVs) stride += 2;
    if (hasColors) stride += 4;
    if (hasTangents) stride += 4;

    std::vector<float> interleaved(vertCount * stride);
    for (size_t i = 0; i < vertCount; i++) {
        size_t off = i * stride;
        interleaved[off + 0] = mesh.positions[i * 3 + 0];
        interleaved[off + 1] = mesh.positions[i * 3 + 1];
        interleaved[off + 2] = mesh.positions[i * 3 + 2];
        size_t at = 3;
        if (hasNormals) {
            interleaved[off + at + 0] = mesh.normals[i * 3 + 0];
            interleaved[off + at + 1] = mesh.normals[i * 3 + 1];
            interleaved[off + at + 2] = mesh.normals[i * 3 + 2];
            at += 3;
        }
        if (hasUVs) {
            interleaved[off + at + 0] = mesh.uvs[i * 2 + 0];
            interleaved[off + at + 1] = mesh.uvs[i * 2 + 1];
            at += 2;
        }
        if (hasColors) {
            interleaved[off + at + 0] = mesh.colors[i * 4 + 0];
            interleaved[off + at + 1] = mesh.colors[i * 4 + 1];
            interleaved[off + at + 2] = mesh.colors[i * 4 + 2];
            interleaved[off + at + 3] = mesh.colors[i * 4 + 3];
            at += 4;
        }
        if (hasTangents) {
            interleaved[off + at + 0] = mesh.tangents[i * 4 + 0];
            interleaved[off + at + 1] = mesh.tangents[i * 4 + 1];
            interleaved[off + at + 2] = mesh.tangents[i * 4 + 2];
            interleaved[off + at + 3] = mesh.tangents[i * 4 + 3];
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 interleaved.size() * sizeof(float),
                 interleaved.data(), GL_STATIC_DRAW);

    GLsizei byteStride = (GLsizei)(stride * sizeof(float));

    // Attribute 0: position (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, byteStride, (void*)0);

    // Attribute 1: normal (vec3)
    if (hasNormals) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, byteStride,
                              (void*)(3 * sizeof(float)));
    } else {
        glDisableVertexAttribArray(1);
    }

    // Attribute 2: uv (vec2)
    if (hasUVs) {
        size_t uvOffset = 3 + (hasNormals ? 3 : 0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, byteStride,
                              (void*)(uvOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(2);
    }

    // Attribute 3: color (vec4)
    if (hasColors) {
        size_t colorOffset = 3 + (hasNormals ? 3 : 0) + (hasUVs ? 2 : 0);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, byteStride,
                              (void*)(colorOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(3);
    }

    // Attribute 4: tangent (vec4, xyz + handedness)
    if (hasTangents) {
        size_t tanOffset = 3 + (hasNormals ? 3 : 0) + (hasUVs ? 2 : 0) + (hasColors ? 4 : 0);
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, byteStride,
                              (void*)(tanOffset * sizeof(float)));
    } else {
        glDisableVertexAttribArray(4);
    }

    // Index buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 mesh.indices.size() * sizeof(uint32_t),
                 mesh.indices.data(), GL_STATIC_DRAW);
    indexCount = (GLsizei)mesh.indices.size();

    glBindVertexArray(0);
}

void MeshNode::uploadToGPU() {
    if (mesh_.empty()) return;
    uploadInterleavedMesh(mesh_, vao_, vbo_, ibo_, indexCount_);
    hasVertexColors_ = mesh_.hasColors();
    gpuDirty_ = false;
    flushPendingTextures();
}

void MeshNode::onRender(SceneGraph& graph) {
    drawRaw();
}

bool MeshNode::drawRaw() {
    flushDeadLodBuffers();   // GL thread — replaced LOD chains free here

    // LOD chain: the selected level (SceneGraph's per-frame distance pass)
    // replaces the base mesh. Every caller — color pass and depth-only
    // shadow pass alike — goes through drawRaw, so all passes draw the SAME
    // level each frame by construction.
    if (!lods_.empty()) {
        LodEntry& e = lods_[(size_t)lodSelected_];
        if (e.mesh.empty()) return false;
        if (e.gpuDirty) {
            uploadInterleavedMesh(e.mesh, e.vao, e.vbo, e.ibo, e.indexCount);
            e.gpuDirty = false;
        }
        flushPendingTextures();
        if (!e.vao || e.indexCount == 0) return false;
        glBindVertexArray(e.vao);
        if (drawMode_ == DrawMode::Lines) {
            glLineWidth(lineWidth_);
            glDrawElements(GL_LINES, e.indexCount, GL_UNSIGNED_INT, nullptr);
        } else {
            glDrawElements(GL_TRIANGLES, e.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(0);
        return true;
    }

    if (mesh_.empty()) return false;
    if (gpuDirty_) uploadToGPU();
    else flushPendingTextures();  // texture-only changes after geometry upload
    if (!vao_ || indexCount_ == 0) return false;

    glBindVertexArray(vao_);
    if (drawMode_ == DrawMode::Lines) {
        glLineWidth(lineWidth_);
        glDrawElements(GL_LINES, indexCount_, GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    return true;
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
