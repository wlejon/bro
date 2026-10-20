#pragma once

#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromesh/gaussian_splat.h>


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
/// evaluated from the spherical-harmonic coefficients on the same pass. It
/// draws with its own pipeline (distinct from the mesh/instanced ones) in a
/// dedicated splat pass (pass_gaussian_splat) with depth-test on,
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

    /// Largest per-axis std-dev across all splats (node-local units — splat
    /// centers/scales live in cloud space and are taken through the node's
    /// world matrix by the splat pipeline). Culling pads the center bounds by
    /// a multiple of this to cover the 3-sigma quads the vertex shader emits;
    /// transforming the padded box by the world matrix scales the pad along
    /// with the sigmas.
    float maxSigma() const { return maxSigma_; }


    const std::vector<float>& instanceData() const { return instanceData_; }
    bool needsResort(const float* view16, const float eye[3],
                     const bromath::Mat4& model) const;
    void resort(const float* view16, const float eye[3],
                const bromath::Mat4& model);

private:
    void refreshBounds();

    bromesh::GaussianSplatCloud cloud_;
    bromath::AABB3 bounds_{};
    float maxSigma_ = 0.0f;
    bool cloudDirty_ = false;

    // Per-splat instance record uploaded in sorted order:
    //   center.xyz (3) | scale.xyz (3) | quat.xyzw (4) | rgba (4) = 14 floats.
    static constexpr int kInstFloats = 14;
    std::vector<float> instanceData_;
    std::vector<uint32_t> order_;   // splat indices, back-to-front
    std::vector<float> depthKey_;   // scratch: view-space depth per splat

    // Camera + node-transform state at last sort, to skip re-sorting when the
    // view and the node are static.
    float lastEye_[3] = {0, 0, 0};
    float lastFwd_[3] = {0, 0, 0};
    float lastModel_[16] = {0};
    bool sorted_ = false;
};

} // namespace bro::scene
