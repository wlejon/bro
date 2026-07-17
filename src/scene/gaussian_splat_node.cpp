#include "scene/gaussian_splat_node.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Shaders. EWA splatting: the vertex shader projects each splat's 3D
// covariance to a 2D screen-space conic and expands the instanced quad along
// the conic's principal axes; the fragment shader evaluates the anisotropic
// Gaussian and outputs premultiplied color.
// ---------------------------------------------------------------------------

static const char* kVert = R"GLSL(#version 330 core
layout(location = 0) in vec2 aCorner;   // quad corner in [-1,1]^2
layout(location = 1) in vec3 aCenter;   // node-local splat center
layout(location = 2) in vec3 aScale;    // linear std-dev along local axes
layout(location = 3) in vec4 aQuat;     // orientation, xyzw
layout(location = 4) in vec4 aColor;    // rgb (view-evaluated) + opacity

// Node world matrix. Rigid transforms + UNIFORM scale only: mat3(uModel)
// rotates the splat orientation/covariance and scales sigma by the uniform
// factor. Non-uniform scale is NOT supported for splats — the CPU back-to-
// front sort and the frustum-cull bounds both assume the transform preserves
// distance ordering / isotropic sigma padding.
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;
uniform vec2 uFocal;     // focal length in pixels (fx, fy)
uniform vec2 uViewport;  // target size in pixels

out vec2 vPos;           // position in std-dev units along principal axes
out vec4 vColor;

const float kSigma = 3.0; // quad covers +/- 3 std

mat3 quatToMat3(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    return mat3(
        1.0-2.0*(yy+zz), 2.0*(xy+wz),     2.0*(xz-wy),     // col 0
        2.0*(xy-wz),     1.0-2.0*(xx+zz), 2.0*(yz+wx),     // col 1
        2.0*(xz+wy),     2.0*(yz-wx),     1.0-2.0*(xx+yy)); // col 2
}

void main() {
    vec4 cam = uView * (uModel * vec4(aCenter, 1.0));
    vec4 clip = uProj * cam;
    // Cull splats behind the camera (GL view space looks down -z).
    if (cam.z > -0.01 || clip.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // offscreen
        return;
    }

    // 3D covariance in world space: Sigma = (Rm R S)(Rm R S)^T where
    // Rm = mat3(uModel) is the node rotation * uniform scale.
    mat3 R = quatToMat3(aQuat);
    mat3 S = mat3(aScale.x, 0.0, 0.0,
                  0.0, aScale.y, 0.0,
                  0.0, 0.0, aScale.z);
    mat3 M = mat3(uModel) * (R * S);
    mat3 Sigma = M * transpose(M);

    // Jacobian of the perspective projection at the view-space center.
    float zz = cam.z * cam.z;
    mat3 J = mat3(
        uFocal.x / cam.z, 0.0, 0.0,                                  // col 0
        0.0, uFocal.y / cam.z, 0.0,                                  // col 1
        -(uFocal.x * cam.x) / zz, -(uFocal.y * cam.y) / zz, 0.0);    // col 2

    mat3 W = mat3(uView);          // world->view rotation
    mat3 T = J * W;
    mat3 cov = T * Sigma * transpose(T);

    // 2x2 screen covariance + low-pass dilation (keeps sub-pixel splats visible).
    float a = cov[0][0] + 0.3;
    float b = cov[1][0];
    float d = cov[1][1] + 0.3;
    float det = a * d - b * b;
    if (det <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }

    // Eigen-decompose to get the principal-axis std-devs (in pixels).
    float mid = 0.5 * (a + d);
    float disc = sqrt(max(0.0, mid * mid - det));
    float l1 = mid + disc;
    float l2 = mid - disc;
    // Eigenvector of [[a,b],[b,d]] for l1, taken from whichever row of
    // (Sigma - l1*I) is better conditioned: row 0 gives (b, l1 - a), row 1
    // gives (l1 - d, b). When the major axis is near the x-axis l1 ~ a and
    // (l1 - a) suffers catastrophic cancellation — for an elongated splat
    // with tiny-but-nonzero b the axis can rotate arbitrarily (up to an
    // effective axis swap), so pick the row with the larger residual.
    // When the screen covariance is isotropic (b == 0, a == d) rounding can
    // still make the chosen row the zero vector — normalize() would return
    // NaN and the splat would silently vanish. Fall back to the
    // axis-aligned eigenvectors in that case.
    vec2 ev = abs(l1 - a) >= abs(l1 - d) ? vec2(b, l1 - a) : vec2(l1 - d, b);
    vec2 e1 = dot(ev, ev) > 0.0 ? normalize(ev)
                                : (a >= d ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    vec2 e2 = vec2(-e1.y, e1.x);
    vec2 axisMajor = e1 * (kSigma * sqrt(l1));
    vec2 axisMinor = e2 * (kSigma * sqrt(max(0.0, l2)));

    // Pixel offset -> NDC offset (NDC spans 2 units across the viewport).
    vec2 offsetPx = aCorner.x * axisMajor + aCorner.y * axisMinor;
    vec2 offsetNdc = offsetPx * 2.0 / uViewport;

    vec3 ndc = clip.xyz / clip.w;
    gl_Position = vec4(ndc.xy + offsetNdc, ndc.z, 1.0);

    vPos = aCorner * kSigma;
    vColor = aColor;
}
)GLSL";

static const char* kFrag = R"GLSL(#version 330 core
in vec2 vPos;
in vec4 vColor;
out vec4 fragColor;

void main() {
    // Anisotropic Gaussian falloff: vPos is already in std-dev units.
    float power = -0.5 * dot(vPos, vPos);
    float alpha = exp(power) * vColor.a;
    if (alpha < (1.0 / 255.0)) discard;
    // Premultiplied "over" to match the billboard/Skia composite path.
    fragColor = vec4(vColor.rgb * alpha, alpha);
}
)GLSL";

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

GaussianSplatNode::~GaussianSplatNode() { releaseGL(); }

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

void GaussianSplatNode::releaseGL() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (quadVbo_) { glDeleteBuffers(1, &quadVbo_); quadVbo_ = 0; }
    if (instVbo_) { glDeleteBuffers(1, &instVbo_); instVbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    instVboCapacity_ = 0;
}

static GLuint compileSplatShader(GLenum stage, const char* src) {
    GLuint s = glCreateShader(stage);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("GaussianSplatNode shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void GaussianSplatNode::ensureProgram() {
    if (program_) return;
    GLuint vs = compileSplatShader(GL_VERTEX_SHADER, kVert);
    GLuint fs = compileSplatShader(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return; }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        LOG_ERROR("GaussianSplatNode link failed: %s", log);
        glDeleteProgram(program_);
        program_ = 0;
        return;
    }
    uModel_ = glGetUniformLocation(program_, "uModel");
    uView_ = glGetUniformLocation(program_, "uView");
    uProj_ = glGetUniformLocation(program_, "uProj");
    uFocal_ = glGetUniformLocation(program_, "uFocal");
    uViewport_ = glGetUniformLocation(program_, "uViewport");
}

void GaussianSplatNode::uploadGeometry() {
    if (!vao_) glGenVertexArrays(1, &vao_);
    if (!quadVbo_) glGenBuffers(1, &quadVbo_);
    if (!instVbo_) glGenBuffers(1, &instVbo_);

    glBindVertexArray(vao_);

    // Static unit quad as a triangle strip: corners in [-1,1]^2.
    static const float quad[8] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f,
    };
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    // Per-splat instance attributes (locations 1..4), divisor 1.
    glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
    GLsizei stride = kInstFloats * sizeof(float);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(0 * sizeof(float)));
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(10 * sizeof(float)));
    glVertexAttribDivisor(4, 1);

    glBindVertexArray(0);
    cloudDirty_ = false;
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

void GaussianSplatNode::resortAndUpload(const float* view16, const float eye[3],
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

    glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
    size_t bytes = instanceData_.size() * sizeof(float);
    if (bytes > instVboCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, instanceData_.data(), GL_DYNAMIC_DRAW);
        instVboCapacity_ = bytes;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, instanceData_.data());
    }

    std::memcpy(lastEye_, eye, sizeof(lastEye_));
    lastFwd_[0] = view16[2]; lastFwd_[1] = view16[6]; lastFwd_[2] = view16[10];
    std::memcpy(lastModel_, model.data, sizeof(lastModel_));
    sorted_ = true;
}

bool GaussianSplatNode::draw(const float* view16, const float* proj16,
                             const float eye[3], int vpW, int vpH) {
    if (cloud_.empty() || vpW <= 0 || vpH <= 0) return false;
    ensureProgram();
    if (!program_) return false;
    if (cloudDirty_ || !vao_) uploadGeometry();
    const bromath::Mat4& model = worldMatrix();
    if (needsResort(view16, eye, model)) resortAndUpload(view16, eye, model);
    if (instanceData_.empty()) return false;

    glUseProgram(program_);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.data);
    glUniformMatrix4fv(uView_, 1, GL_FALSE, view16);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj16);
    // Focal length in pixels from the projection's (0,0)/(1,1) (col-major).
    float fx = 0.5f * static_cast<float>(vpW) * proj16[0];
    float fy = 0.5f * static_cast<float>(vpH) * proj16[5];
    glUniform2f(uFocal_, fx, fy);
    glUniform2f(uViewport_, static_cast<float>(vpW), static_cast<float>(vpH));

    glBindVertexArray(vao_);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(cloud_.count()));
    glBindVertexArray(0);
    return true;
}

} // namespace bro::scene
