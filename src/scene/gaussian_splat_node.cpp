#include "scene/gaussian_splat_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {





// ---------------------------------------------------------------------------
// Spherical-harmonic evaluation (real SH up to degree 3), INRIA convention.
// Coefficients are interleaved RGB per coefficient: sh[k*3 + channel].
// ---------------------------------------------------------------------------
namespace {

constexpr float C0 = 0.28209479177387814f;
constexpr float C1 = 0.4886025119029199f;
constexpr float C2[5] = {1.0925484305920792f, -1.0925484305920792f,
                         0.31539156525252005f, -1.0925484305920792f,
                         0.5462742152960396f};
constexpr float C3[7] = {-0.5900435899266435f, 2.890611442640554f,
                         -0.4570457994644658f, 0.3731763325901154f,
                         -0.4570457994644658f, 1.445305721320277f,
                         -0.5900435899266435f};

} // namespace

GaussianSplatNode::GaussianSplatNode(const std::string& name) : SceneNode(name) {}

GaussianSplatNode::~GaussianSplatNode() = default;

void GaussianSplatNode::setCloud(const bromesh::GaussianSplatCloud& cloud) {
    cloud_ = cloud;
    refreshBounds();
    cloudDirty_ = true;
    sorted_ = false;
}

void GaussianSplatNode::setCloud(bromesh::GaussianSplatCloud&& cloud) {
    cloud_ = std::move(cloud);
    refreshBounds();
    cloudDirty_ = true;
    sorted_ = false;
}

void GaussianSplatNode::refreshBounds() {
    bounds_ = cloud_.bounds();
    maxSigma_ = 0.0f;
    for (float s : cloud_.scales) {
        maxSigma_ = std::max(maxSigma_, std::fabs(s));
    }
}

bool GaussianSplatNode::needsResort(const float* view16, const float eye[3],
                                    const bromath::Mat4& model) const {
    if (!sorted_) return true;
    // Node moved/rotated/scaled since the last sort — depth order and the
    // SH view directions both depend on the world transform.
    if (std::memcmp(lastModel_, model.data, sizeof(lastModel_)) != 0) return true;
    // View-space Z axis in world coords = third row of the rotation (col-major).
    const float fwd[3] = {view16[2], view16[6], view16[10]};
    float de = 0, df = 0;
    for (int i = 0; i < 3; ++i) {
        de += (eye[i] - lastEye_[i]) * (eye[i] - lastEye_[i]);
        df += (fwd[i] - lastFwd_[i]) * (fwd[i] - lastFwd_[i]);
    }
    return de > 1e-6f || df > 1e-8f;
}

void GaussianSplatNode::resort(const float* view16, const float eye[3],
                                const bromath::Mat4& model) {
    const size_t n = cloud_.count();
    if (n == 0) return;

    // View-space depth key of the transformed center, computed without
    // transforming each center: (viewRow2 . (A p + t)) = ((A^T viewRow2) . p)
    // + (viewRow2 . t). Pull the view's depth row back into node space once
    // and keep one dot product per splat. At an identity model matrix this is
    // exactly the old world-space key (bit-identical output).
    const float vz[3] = {view16[2], view16[6], view16[10]};
    float axis[3], bias;
    for (int j = 0; j < 3; ++j) {
        axis[j] = model.at(0, j) * vz[0] + model.at(1, j) * vz[1] + model.at(2, j) * vz[2];
    }
    bias = view16[14] + (vz[0] * model.at(0, 3) + vz[1] * model.at(1, 3) + vz[2] * model.at(2, 3));

    // Camera position in node space, for the SH view direction. The stored SH
    // coefficients live in cloud (node-local) space, so the view direction is
    // evaluated there too — under a rigid + uniform-scale transform this is
    // the world direction rotated into the cloud's frame.
    const bromath::Mat4 invModel = bromath::minverse(model);
    const bromath::Vec3 eyeLocalV = bromath::mtransformPoint(
        invModel, bromath::Vec3{eye[0], eye[1], eye[2]});
    const float eyeLocal[3] = {eyeLocalV.x, eyeLocalV.y, eyeLocalV.z};

    depthKey_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const float* p = &cloud_.positions[i * 3];
        depthKey_[i] = axis[0] * p[0] + axis[1] * p[1] + axis[2] * p[2] + bias;
    }

    order_.resize(n);
    for (size_t i = 0; i < n; ++i) order_[i] = static_cast<uint32_t>(i);
    // Back-to-front: most-negative view z (farthest) first.
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        return depthKey_[a] < depthKey_[b];
    });

    // Build the sorted instance buffer, evaluating SH -> RGB per splat for the
    // current view direction.
    const int degree = std::min(3, std::max(0, cloud_.shDegree));
    const int stride = cloud_.shStride();
    instanceData_.resize(n * kInstFloats);
    for (size_t k = 0; k < n; ++k) {
        const uint32_t i = order_[k];
        const float* pos = &cloud_.positions[i * 3];
        const float* scl = &cloud_.scales[i * 3];
        const float* rot = &cloud_.rotations[i * 4];
        const float* sh = &cloud_.sh[i * static_cast<size_t>(stride)];

        // View direction from camera to splat (unit), in cloud space.
        float dx = pos[0] - eyeLocal[0], dy = pos[1] - eyeLocal[1], dz = pos[2] - eyeLocal[2];
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-8f) { dx = 0; dy = 0; dz = 1; } else { dx /= len; dy /= len; dz /= len; }

        float rgb[3];
        for (int c = 0; c < 3; ++c) {
            float r = C0 * sh[0 * 3 + c];
            if (degree >= 1) {
                r += -C1 * dy * sh[1 * 3 + c] + C1 * dz * sh[2 * 3 + c] - C1 * dx * sh[3 * 3 + c];
            }
            if (degree >= 2) {
                float xx = dx * dx, yy = dy * dy, zz = dz * dz;
                float xy = dx * dy, yz = dy * dz, xz = dx * dz;
                r += C2[0] * xy * sh[4 * 3 + c] + C2[1] * yz * sh[5 * 3 + c] +
                     C2[2] * (2.0f * zz - xx - yy) * sh[6 * 3 + c] +
                     C2[3] * xz * sh[7 * 3 + c] + C2[4] * (xx - yy) * sh[8 * 3 + c];
            }
            if (degree >= 3) {
                float xx = dx * dx, yy = dy * dy, zz = dz * dz;
                float xy = dx * dy, yz = dy * dz, xz = dx * dz;
                r += C3[0] * dy * (3.0f * xx - yy) * sh[9 * 3 + c] +
                     C3[1] * xy * dz * sh[10 * 3 + c] +
                     C3[2] * dy * (4.0f * zz - xx - yy) * sh[11 * 3 + c] +
                     C3[3] * dz * (2.0f * zz - 3.0f * xx - 3.0f * yy) * sh[12 * 3 + c] +
                     C3[4] * dx * (4.0f * zz - xx - yy) * sh[13 * 3 + c] +
                     C3[5] * dz * (xx - yy) * sh[14 * 3 + c] +
                     C3[6] * dx * (xx - 3.0f * yy) * sh[15 * 3 + c];
            }
            rgb[c] = std::max(0.0f, r + 0.5f);
        }

        float* o = &instanceData_[k * kInstFloats];
        o[0] = pos[0]; o[1] = pos[1]; o[2] = pos[2];
        o[3] = scl[0]; o[4] = scl[1]; o[5] = scl[2];
        o[6] = rot[0]; o[7] = rot[1]; o[8] = rot[2]; o[9] = rot[3];
        o[10] = rgb[0]; o[11] = rgb[1]; o[12] = rgb[2]; o[13] = cloud_.opacities[i];
    }

    std::memcpy(lastEye_, eye, sizeof(lastEye_));
    lastFwd_[0] = view16[2]; lastFwd_[1] = view16[6]; lastFwd_[2] = view16[10];
    std::memcpy(lastModel_, model.data, sizeof(lastModel_));
    sorted_ = true;
}

bool GaussianSplatNode::draw(const float*, const float*,
                             const float*, int, int) {
    return false;
}

} // namespace bro::scene
