#pragma once

#include "scene/scene_node.h"
#include <bromesh/mesh_data.h>
#include <glad/gl.h>

#include <vector>

namespace bro::scene {

/// A renderable 3D mesh node. Holds bromesh::MeshData and owns GL resources.
/// Renders into a shared FBO owned by SceneGraph (set up during render pass).
class MeshNode : public SceneNode {
public:
    explicit MeshNode(const std::string& name = "");
    ~MeshNode() override;

    MeshNode(const MeshNode&) = delete;
    MeshNode& operator=(const MeshNode&) = delete;

    Type type() const override { return Type::Mesh; }
    void onRender(SceneGraph& graph) override;

    // --- Mesh data ---

    /// Set mesh geometry. Uploads to GPU on next render.
    void setMesh(const bromesh::MeshData& mesh);
    void setMesh(bromesh::MeshData&& mesh);
    const bromesh::MeshData& mesh() const { return mesh_; }

    // --- Material ---

    void setColor(float r, float g, float b, float a = 1.0f) {
        color_[0] = r; color_[1] = g; color_[2] = b; color_[3] = a;
    }
    const float* color() const { return color_; }

    /// Release GPU resources (call before GL context is destroyed).
    void releaseGL();

private:
    void uploadToGPU();

    bromesh::MeshData mesh_;
    bool gpuDirty_ = false;

    // GL resources
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLsizei indexCount_ = 0;

    // Material
    float color_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace bro::scene
