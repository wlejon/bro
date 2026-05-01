#include "scene/instanced_mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/manipulation/normals.h>
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
    bounds_ = mesh_.empty() ? bromesh::BBox{} : bromesh::computeBBox(mesh_);
}

void InstancedMeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    ensureTangents(mesh_);
    meshDirty_ = true;
    bounds_ = mesh_.empty() ? bromesh::BBox{} : bromesh::computeBBox(mesh_);
}

void InstancedMeshNode::setInstances(const float* data, size_t count) {
    instanceData_.assign(data, data + count * 16);
    instanceCount_ = count;
    instancesDirty_ = true;
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
}

void InstancedMeshNode::updateInstance(size_t i, const float* data16) {
    if (i >= instanceCount_) return;
    std::memcpy(instanceData_.data() + i * 16, data16, sizeof(float) * 16);
    instancesDirty_ = true;
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

static void flushTex(InstancedMeshNode::PendingTex& p, GLuint& glTex) {
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

void InstancedMeshNode::uploadMeshToGPU() {
    if (mesh_.empty()) return;

    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);
    if (!ibo_) glGenBuffers(1, &ibo_);

    glBindVertexArray(vao_);

    size_t vertCount = mesh_.vertexCount();
    bool hasNormals = mesh_.hasNormals();
    bool hasUVs = mesh_.hasUVs();
    bool hasColors = mesh_.hasColors();
    bool hasTangents = mesh_.hasTangents();
    hasVertexColors_ = hasColors;

    size_t stride = 3;
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
                 mesh_.indices.size() * sizeof(uint32_t),
                 mesh_.indices.data(), GL_STATIC_DRAW);
    indexCount_ = (GLsizei)mesh_.indices.size();

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
    if (!instVbo_) glGenBuffers(1, &instVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
    size_t bytes = instanceData_.size() * sizeof(float);
    if (bytes > instVboCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, instanceData_.data(), GL_DYNAMIC_DRAW);
        instVboCapacity_ = bytes;
    } else if (bytes > 0) {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, instanceData_.data());
    }
    instancesDirty_ = false;
}

void InstancedMeshNode::onRender(SceneGraph& graph) {
    (void)graph;
    drawRawInstanced();
}

bool InstancedMeshNode::drawRawInstanced() {
    if (mesh_.empty() || instanceCount_ == 0) return false;
    if (meshDirty_) uploadMeshToGPU();
    if (instancesDirty_) uploadInstancesToGPU();
    if (!vao_ || indexCount_ == 0) return false;

    glBindVertexArray(vao_);
    glDrawElementsInstanced(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr,
                            (GLsizei)instanceCount_);
    glBindVertexArray(0);
    return true;
}

} // namespace bro::scene
