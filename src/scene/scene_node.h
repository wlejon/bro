#pragma once

#include <cmath>
#include <cstdint>
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

    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

/// 2D affine transform matrix (3x3, stored as 6 floats for 2D use).
/// | a  b  tx |
/// | c  d  ty |
/// | 0  0  1  |
struct Mat3 {
    float a = 1, b = 0, tx = 0;
    float c = 0, d = 1, ty = 0;

    static Mat3 identity() { return {}; }

    static Mat3 translate(float x, float y) {
        return {1, 0, x, 0, 1, y};
    }

    static Mat3 scale(float sx, float sy) {
        return {sx, 0, 0, 0, sy, 0};
    }

    static Mat3 rotate(float radians) {
        float cs = std::cos(radians), sn = std::sin(radians);
        return {cs, -sn, 0, sn, cs, 0};
    }

    Mat3 operator*(const Mat3& o) const {
        return {
            a * o.a + b * o.c,
            a * o.b + b * o.d,
            a * o.tx + b * o.ty + tx,
            c * o.a + d * o.c,
            c * o.b + d * o.d,
            c * o.tx + d * o.ty + ty
        };
    }

    Vec3 transformPoint(const Vec3& p) const {
        return {a * p.x + b * p.y + tx,
                c * p.x + d * p.y + ty,
                p.z};
    }
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
    float rotation() const { return rotation_; }
    const Vec3& scale() const { return scale_; }

    void setPosition(float x, float y, float z = 0);
    void setRotation(float radians);
    void setScale(float sx, float sy);

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
    const Mat3& localMatrix() const;
    const Mat3& worldMatrix() const;

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
    float rotation_ = 0;
    Vec3 scale_{1, 1, 1};
    bool visible_ = true;

    SceneNode* parent_ = nullptr;
    std::vector<SceneNode*> children_;

    mutable Mat3 localMatrix_;
    mutable Mat3 worldMatrix_;
    mutable bool localDirty_ = true;
    mutable bool dirty_ = true;

    static uint32_t s_nextId;
};

} // namespace bro::scene
