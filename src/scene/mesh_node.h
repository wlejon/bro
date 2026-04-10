#pragma once

#include "scene/scene_node.h"
#include <bromesh/mesh_data.h>
#include <bromesh/analysis/bvh.h>
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

    /// Set mesh geometry. Uploads to GPU on next render and invalidates the
    /// cached BVH (rebuilt lazily on the next raycast).
    void setMesh(const bromesh::MeshData& mesh);
    void setMesh(bromesh::MeshData&& mesh);
    const bromesh::MeshData& mesh() const { return mesh_; }

    /// Local-space AABB of the current mesh. Cached; updated in setMesh.
    /// Returns an empty box for empty meshes.
    const bromesh::BBox& localBounds() const { return bounds_; }

    /// Lazily-built, cached BVH over the current mesh. Built on first call
    /// after setMesh and reused until the next setMesh. Used by scene.raycast
    /// to avoid O(N) ray-triangle tests on dense meshes (terrain chunks etc.).
    const bromesh::MeshBVH& bvh() const;

    // --- Material ---

    void setColor(float r, float g, float b, float a = 1.0f) {
        color_[0] = r; color_[1] = g; color_[2] = b; color_[3] = a;
    }
    const float* color() const { return color_; }

    void setEmissive(float e) { emissive_ = e; }
    float emissive() const { return emissive_; }

    /// Polygon offset (forwarded to glPolygonOffset before drawing this mesh).
    /// Negative `units` pulls the surface forward in the depth buffer, useful
    /// for layering co-located meshes (e.g. high-detail LOD meshes that should
    /// always win the depth test against lower-detail backdrops).
    /// Set both to 0 to disable.
    void setDepthBias(float factor, float units) {
        depthBiasFactor_ = factor;
        depthBiasUnits_ = units;
    }
    float depthBiasFactor() const { return depthBiasFactor_; }
    float depthBiasUnits() const { return depthBiasUnits_; }

    /// Release GPU resources (call before GL context is destroyed).
    void releaseGL();

private:
    void uploadToGPU();

    bromesh::MeshData mesh_;
    bool gpuDirty_ = false;

    // Cached bounds + BVH. Both are invalidated (bvhDirty_ = true, bounds_
    // recomputed) on every setMesh call.
    bromesh::BBox bounds_;
    mutable bromesh::MeshBVH bvh_;
    mutable bool bvhDirty_ = true;

    // GL resources
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLsizei indexCount_ = 0;

    // Material
    float color_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive_ = 0.0f;

    // Polygon offset (per-mesh depth bias for layered LOD meshes)
    float depthBiasFactor_ = 0.0f;
    float depthBiasUnits_ = 0.0f;
};

} // namespace bro::scene
