#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/analysis/bbox.h>
#include <bromesh/manipulation/normals.h>

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
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
}

void MeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    gpuDirty_ = true;
    bvhDirty_ = true;
    bounds_ = mesh_.empty() ? bromath::AABB3{} : bromesh::computeBBox(mesh_);
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

void MeshNode::uploadToGPU() {
    if (mesh_.empty()) return;

    // Create GL objects if needed
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);
    if (!ibo_) glGenBuffers(1, &ibo_);

    glBindVertexArray(vao_);

    // Interleave: pos(3) + normal(3) + uv(2) + color(4) + tangent(4)
    size_t vertCount = mesh_.vertexCount();
    bool hasNormals = mesh_.hasNormals();
    bool hasUVs = mesh_.hasUVs();
    bool hasColors = mesh_.hasColors();
    bool hasTangents = mesh_.hasTangents();
    hasVertexColors_ = hasColors;

    size_t stride = 3; // position always
    if (hasNormals) stride += 3;
    if (hasUVs) stride += 2;
    if (hasColors) stride += 4;
    if (hasTangents) stride += 4;

    std::vector<float> interleaved(vertCount * stride);
    for (size_t i = 0; i < vertCount; i++) {
        size_t off = i * stride;
        interleaved[off + 0] = mesh_.positions[i * 3 + 0];
        interleaved[off + 1] = mesh_.positions[i * 3 + 1];
        interleaved[off + 2] = mesh_.positions[i * 3 + 2];
        size_t at = 3;
        if (hasNormals) {
            interleaved[off + at + 0] = mesh_.normals[i * 3 + 0];
            interleaved[off + at + 1] = mesh_.normals[i * 3 + 1];
            interleaved[off + at + 2] = mesh_.normals[i * 3 + 2];
            at += 3;
        }
        if (hasUVs) {
            interleaved[off + at + 0] = mesh_.uvs[i * 2 + 0];
            interleaved[off + at + 1] = mesh_.uvs[i * 2 + 1];
            at += 2;
        }
        if (hasColors) {
            interleaved[off + at + 0] = mesh_.colors[i * 4 + 0];
            interleaved[off + at + 1] = mesh_.colors[i * 4 + 1];
            interleaved[off + at + 2] = mesh_.colors[i * 4 + 2];
            interleaved[off + at + 3] = mesh_.colors[i * 4 + 3];
            at += 4;
        }
        if (hasTangents) {
            interleaved[off + at + 0] = mesh_.tangents[i * 4 + 0];
            interleaved[off + at + 1] = mesh_.tangents[i * 4 + 1];
            interleaved[off + at + 2] = mesh_.tangents[i * 4 + 2];
            interleaved[off + at + 3] = mesh_.tangents[i * 4 + 3];
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
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
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 mesh_.indices.size() * sizeof(uint32_t),
                 mesh_.indices.data(), GL_STATIC_DRAW);
    indexCount_ = (GLsizei)mesh_.indices.size();

    glBindVertexArray(0);
    gpuDirty_ = false;

    flushPendingTextures();
}

void MeshNode::onRender(SceneGraph& graph) {
    drawRaw();
}

bool MeshNode::drawRaw() {
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
}

} // namespace bro::scene
