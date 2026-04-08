#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

namespace bro::scene {

struct Vec3 {
    float x = 0, y = 0, z = 0;

    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float lengthSq() const { return x * x + y * y + z * z; }
    Vec3 normalized() const {
        float len = length();
        return (len > 0) ? Vec3{x / len, y / len, z / len} : Vec3{};
    }
};

/// Quaternion (x, y, z, w) for 3D rotations.
/// Convention: w is the scalar part. Identity = (0, 0, 0, 1).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;

    Quat() = default;
    Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quat identity() { return {0, 0, 0, 1}; }

    /// Construct from axis-angle (axis must be normalized).
    static Quat fromAxisAngle(const Vec3& axis, float radians) {
        float half = radians * 0.5f;
        float s = std::sin(half);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
    }

    /// Construct from Euler angles (radians, applied as Z * Y * X — roll, pitch, yaw order).
    static Quat fromEuler(float rx, float ry, float rz) {
        float cx = std::cos(rx * 0.5f), sx = std::sin(rx * 0.5f);
        float cy = std::cos(ry * 0.5f), sy = std::sin(ry * 0.5f);
        float cz = std::cos(rz * 0.5f), sz = std::sin(rz * 0.5f);
        return {
            sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz
        };
    }

    /// Extract Euler angles (radians) from quaternion.
    Vec3 toEuler() const {
        // X (pitch)
        float sinr_cosp = 2.0f * (w * x + y * z);
        float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
        float rx = std::atan2(sinr_cosp, cosr_cosp);
        // Y (yaw)
        float sinp = 2.0f * (w * y - z * x);
        float ry = (std::fabs(sinp) >= 1.0f)
                    ? std::copysign(3.14159265f / 2.0f, sinp)
                    : std::asin(sinp);
        // Z (roll)
        float siny_cosp = 2.0f * (w * z + x * y);
        float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
        float rz = std::atan2(siny_cosp, cosy_cosp);
        return {rx, ry, rz};
    }

    Quat operator*(const Quat& q) const {
        return {
            w * q.x + x * q.w + y * q.z - z * q.y,
            w * q.y - x * q.z + y * q.w + z * q.x,
            w * q.z + x * q.y - y * q.x + z * q.w,
            w * q.w - x * q.x - y * q.y - z * q.z
        };
    }

    Quat conjugate() const { return {-x, -y, -z, w}; }

    Quat normalized() const {
        float len = std::sqrt(x * x + y * y + z * z + w * w);
        return (len > 0) ? Quat{x / len, y / len, z / len, w / len} : Quat{};
    }

    /// Rotate a vector by this quaternion.
    Vec3 rotate(const Vec3& v) const {
        // q * v * q^-1, optimized
        Vec3 qv{x, y, z};
        Vec3 t = qv.cross(v) * 2.0f;
        return v + t * w + qv.cross(t);
    }
};

/// 4x4 column-major matrix for 3D transforms.
/// Memory layout: m[col][row], matching OpenGL/bromesh convention.
struct Mat4 {
    float m[4][4] = {
        {1, 0, 0, 0},
        {0, 1, 0, 0},
        {0, 0, 1, 0},
        {0, 0, 0, 1}
    };

    static Mat4 identity() { return {}; }

    static Mat4 translate(float x, float y, float z) {
        Mat4 r;
        r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
        return r;
    }

    static Mat4 scale(float sx, float sy, float sz) {
        Mat4 r;
        r.m[0][0] = sx; r.m[1][1] = sy; r.m[2][2] = sz;
        return r;
    }

    static Mat4 fromQuat(const Quat& q) {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy + wz);     r.m[0][2] = 2 * (xz - wy);
        r.m[1][0] = 2 * (xy - wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz + wx);
        r.m[2][0] = 2 * (xz + wy);     r.m[2][1] = 2 * (yz - wx);     r.m[2][2] = 1 - 2 * (xx + yy);
        return r;
    }

    /// Compose TRS: translate * rotate * scale.
    static Mat4 trs(const Vec3& pos, const Quat& rot, const Vec3& scl) {
        Mat4 r = fromQuat(rot);
        // Apply scale to rotation columns
        for (int i = 0; i < 3; i++) {
            r.m[0][i] *= scl.x;
            r.m[1][i] *= scl.y;
            r.m[2][i] *= scl.z;
        }
        // Set translation
        r.m[3][0] = pos.x; r.m[3][1] = pos.y; r.m[3][2] = pos.z;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) {
                r.m[c][row] = 0;
                for (int k = 0; k < 4; k++)
                    r.m[c][row] += m[k][row] * b.m[c][k];
            }
        return r;
    }

    Vec3 transformPoint(const Vec3& p) const {
        float w = m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3];
        return {
            (m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0]) / w,
            (m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1]) / w,
            (m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2]) / w
        };
    }

    Vec3 transformDirection(const Vec3& d) const {
        return {
            m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z,
            m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z,
            m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z
        };
    }

    /// Perspective projection matrix (like glFrustum / gluPerspective).
    /// fovY in radians, aspect = width/height.
    static Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
        float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 r{};
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = (farZ + nearZ) / (nearZ - farZ);
        r.m[2][3] = -1.0f;
        r.m[3][2] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    /// Orthographic projection matrix.
    static Mat4 orthographic(float left, float right, float bottom, float top,
                             float nearZ, float farZ) {
        Mat4 r{};
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] = 2.0f / (right - left);
        r.m[1][1] = 2.0f / (top - bottom);
        r.m[2][2] = -2.0f / (farZ - nearZ);
        r.m[3][0] = -(right + left) / (right - left);
        r.m[3][1] = -(top + bottom) / (top - bottom);
        r.m[3][2] = -(farZ + nearZ) / (farZ - nearZ);
        r.m[3][3] = 1.0f;
        return r;
    }

    /// Look-at view matrix (camera at eye, looking at center, with up vector).
    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = (center - eye).normalized();
        Vec3 s = f.cross(up).normalized();
        Vec3 u = s.cross(f);
        Mat4 r{};
        std::memset(r.m, 0, sizeof(r.m));
        r.m[0][0] =  s.x; r.m[1][0] =  s.y; r.m[2][0] =  s.z;
        r.m[0][1] =  u.x; r.m[1][1] =  u.y; r.m[2][1] =  u.z;
        r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
        r.m[3][0] = -s.dot(eye);
        r.m[3][1] = -u.dot(eye);
        r.m[3][2] =  f.dot(eye);
        r.m[3][3] = 1.0f;
        return r;
    }

    /// Access as flat float[16] pointer (column-major, for GL/bromesh).
    const float* data() const { return &m[0][0]; }
    float* data() { return &m[0][0]; }
};

/// Base class for all scene graph nodes.
/// Provides hierarchical transforms (position, rotation, scale) with
/// cached world matrix and dirty propagation.
class SceneNode {
public:
    explicit SceneNode(const std::string& name = "");
    virtual ~SceneNode();

    SceneNode(const SceneNode&) = delete;
    SceneNode& operator=(const SceneNode&) = delete;

    // --- Identity ---
    uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // --- Transform (local space) ---
    const Vec3& position() const { return position_; }
    const Quat& rotation() const { return rotation_; }
    const Vec3& scale() const { return scale_; }

    void setPosition(float x, float y, float z = 0);
    void setPosition(const Vec3& pos);
    void setRotation(const Quat& q);
    /// Convenience: set rotation from Euler angles (radians).
    void setRotationEuler(float rx, float ry, float rz);
    /// Convenience: set rotation around Z axis (2D rotation).
    void setRotationZ(float radians);
    void setScale(float sx, float sy, float sz = 1);
    void setScale(const Vec3& s);

    // --- Visibility ---
    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

    // --- Hierarchy ---
    SceneNode* parent() const { return parent_; }
    const std::vector<SceneNode*>& children() const { return children_; }

    void addChild(SceneNode* child);
    void removeChild(SceneNode* child);
    void removeFromParent();

    // --- World transform ---
    const Mat4& localMatrix() const;
    const Mat4& worldMatrix() const;

    /// Convert a point from local space to world space.
    Vec3 localToWorld(const Vec3& local) const;

    /// Traverse this node and all descendants depth-first.
    void traverse(const std::function<void(SceneNode*)>& fn);

    /// Mark this node (and descendants) as needing world matrix recomputation.
    void markDirty();
    bool isDirty() const { return dirty_; }

    // --- Rendering hook ---
    /// Called by SceneGraph during render traversal. Override in renderable nodes.
    virtual void onRender(class SceneGraph& graph) {}

    // --- Type tag for downcasting ---
    enum class Type : uint8_t { Base, Shape, Sprite, Physics };
    virtual Type type() const { return Type::Base; }

private:
    void updateLocalMatrix() const;
    void updateWorldMatrix() const;

    uint32_t id_;
    std::string name_;

    Vec3 position_;
    Quat rotation_;
    Vec3 scale_{1, 1, 1};
    bool visible_ = true;

    SceneNode* parent_ = nullptr;
    std::vector<SceneNode*> children_;

    mutable Mat4 localMatrix_;
    mutable Mat4 worldMatrix_;
    mutable bool localDirty_ = true;
    mutable bool dirty_ = true;

    static uint32_t s_nextId;
};

} // namespace bro::scene
