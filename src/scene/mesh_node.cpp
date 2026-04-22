#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <bromesh/analysis/bbox.h>

namespace bro::scene {

MeshNode::MeshNode(const std::string& name) : SceneNode(name) {}

MeshNode::~MeshNode() {
    releaseGL();
}

void MeshNode::setMesh(const bromesh::MeshData& mesh) {
    mesh_ = mesh;
    gpuDirty_ = true;
    bvhDirty_ = true;
    bounds_ = mesh_.empty() ? bromesh::BBox{} : bromesh::computeBBox(mesh_);
}

void MeshNode::setMesh(bromesh::MeshData&& mesh) {
    mesh_ = std::move(mesh);
    gpuDirty_ = true;
    bvhDirty_ = true;
    bounds_ = mesh_.empty() ? bromesh::BBox{} : bromesh::computeBBox(mesh_);
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
    indexCount_ = 0;
}

void MeshNode::setBaseColorTexture(int width, int height, const uint8_t* rgba) {
    if (width <= 0 || height <= 0 || !rgba) {
        clearBaseColorTexture();
        return;
    }
    pendingTexData_.assign(rgba, rgba + (size_t)width * (size_t)height * 4);
    pendingTexW_ = width;
    pendingTexH_ = height;
    textureDirty_ = true;
}

void MeshNode::clearBaseColorTexture() {
    pendingTexData_.clear();
    pendingTexW_ = 0;
    pendingTexH_ = 0;
    textureDirty_ = true;
    // Actual glDeleteTextures is deferred to uploadToGPU / releaseGL so it
    // runs on the render thread with a GL context bound.
}

void MeshNode::uploadToGPU() {
    if (mesh_.empty()) return;

    // Create GL objects if needed
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!vbo_) glGenBuffers(1, &vbo_);
    if (!ibo_) glGenBuffers(1, &ibo_);

    glBindVertexArray(vao_);

    // Interleave: pos(3) + normal(3) + uv(2) + color(4)
    size_t vertCount = mesh_.vertexCount();
    bool hasNormals = mesh_.hasNormals();
    bool hasUVs = mesh_.hasUVs();
    bool hasColors = mesh_.hasColors();
    hasVertexColors_ = hasColors;

    size_t stride = 3; // position always
    if (hasNormals) stride += 3;
    if (hasUVs) stride += 2;
    if (hasColors) stride += 4;

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

    // Index buffer
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 mesh_.indices.size() * sizeof(uint32_t),
                 mesh_.indices.data(), GL_STATIC_DRAW);
    indexCount_ = (GLsizei)mesh_.indices.size();

    glBindVertexArray(0);
    gpuDirty_ = false;

    if (textureDirty_) {
        if (pendingTexW_ > 0 && pendingTexH_ > 0 && !pendingTexData_.empty()) {
            if (!texture_) glGenTextures(1, &texture_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                         pendingTexW_, pendingTexH_, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pendingTexData_.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, 0);
            // Free staged bytes — GL owns them now.
            pendingTexData_.clear();
            pendingTexData_.shrink_to_fit();
        } else if (texture_) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        textureDirty_ = false;
    }
}

void MeshNode::onRender(SceneGraph& graph) {
    drawRaw();
}

bool MeshNode::drawRaw() {
    if (mesh_.empty()) return false;
    if (gpuDirty_) uploadToGPU();
    if (!vao_ || indexCount_ == 0) return false;

    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    return true;
}

} // namespace bro::scene
