#pragma once

#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromesh/gaussian_splat.h>
#include <glad/gl.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

/// Renders a 3D Gaussian Splat cloud (bromesh::GaussianSplatCloud) with EWA
/// splatting: each splat's 3D covariance is projected to a screen-space 2D
/// conic in the vertex shader and evaluated as an anisotropic Gaussian in the
/// fragment shader. One instanced quad per splat.
///
/// Transparency is order-dependent, so splats are depth-sorted back-to-front
/// on the CPU each time the camera moves enough, and view-dependent color is
/// evaluated from the spherical-harmonic coefficients on the same pass. The
/// node owns its own GL program (a distinct pipeline from the mesh/instanced
/// shaders); SceneGraph drives it in a dedicated splat pass with depth-test on,
/// depth-write off, and premultiplied-over blending.
class GaussianSplatNode : public SceneNode {
public:
    explicit GaussianSplatNode(const std::string& name = "");
    ~GaussianSplatNode() override;

    GaussianSplatNode(const GaussianSplatNode&) = delete;
    GaussianSplatNode& operator=(const GaussianSplatNode&) = delete;

    Type type() const override { return Type::GaussianSplat; }

    // --- Cloud ---
    void setCloud(const bromesh::GaussianSplatCloud& cloud);
    void setCloud(bromesh::GaussianSplatCloud&& cloud);
    const bromesh::GaussianSplatCloud& cloud() const { return cloud_; }
    size_t splatCount() const { return cloud_.count(); }
    const bromath::AABB3& localBounds() const { return bounds_; }

    /// Draw the cloud. Matrices are column-major 4x4 (bromath layout). `eye` is
    /// the world-space camera position; `vpW`/`vpH` the target viewport size in
    /// pixels (the mesh FBO). Called by SceneGraph during the splat pass with
    /// the splat GL state already set. Returns false if nothing was drawn.
    bool draw(const float* view16, const float* proj16,
              const float eye[3], int vpW, int vpH);

private:
    void releaseGL();
    void ensureProgram();
    void uploadGeometry();           // static per-splat attributes -> GPU order buffer
    void resortAndUpload(const float* view16, const float eye[3]);
    bool cameraMovedSince(const float* view16, const float eye[3]) const;

    bromesh::GaussianSplatCloud cloud_;
    bromath::AABB3 bounds_{};
    bool cloudDirty_ = false;

    // GL program (lazily compiled, shared shape but per-node owned for now).
    GLuint program_ = 0;
    GLint uView_ = -1, uProj_ = -1, uFocal_ = -1, uViewport_ = -1;

    // Geometry: a unit quad (4 corners) drawn instanced once per splat.
    GLuint vao_ = 0;
    GLuint quadVbo_ = 0;
    GLuint instVbo_ = 0;
    size_t instVboCapacity_ = 0; // bytes

    // Per-splat instance record uploaded in sorted order:
    //   center.xyz (3) | scale.xyz (3) | quat.xyzw (4) | rgba (4) = 14 floats.
    static constexpr int kInstFloats = 14;
    std::vector<float> instanceData_;
    std::vector<uint32_t> order_;   // splat indices, back-to-front
    std::vector<float> depthKey_;   // scratch: view-space depth per splat

    // Camera state at last sort, to skip re-sorting when the view is static.
    float lastEye_[3] = {0, 0, 0};
    float lastFwd_[3] = {0, 0, 0};
    bool sorted_ = false;
};

} // namespace bro::scene
