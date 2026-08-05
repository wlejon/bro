#include "scene/scene_node.h"
#include <algorithm>
#include <cmath>

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

uint32_t SceneNode::s_nextId = 1;

SceneNode::SceneNode(const std::string& name)
    : id_(s_nextId++), name_(name) {}

SceneNode::~SceneNode() {
    // Detach from parent
    if (parent_) {
        auto& siblings = parent_->children_;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), this), siblings.end());
    }
    // Orphan children
    for (auto* child : children_) {
        child->parent_ = nullptr;
    }
}

void SceneNode::setPosition(float x, float y, float z) {
    position_ = {x, y, z};
    localDirty_ = true;
    markDirty();
}

void SceneNode::setPosition(const Vec3& pos) {
    position_ = pos;
    localDirty_ = true;
    markDirty();
}

void SceneNode::setRotation(const Quat& q) {
    rotation_ = q;
    localDirty_ = true;
    markDirty();
}

void SceneNode::setRotationEuler(float rx, float ry, float rz) {
    rotation_ = bromath::qfromEuler(rx, ry, rz);
    localDirty_ = true;
    markDirty();
}

void SceneNode::setRotationZ(float radians) {
    rotation_ = bromath::qaxisAngle({0, 0, 1}, radians);
    localDirty_ = true;
    markDirty();
}

void SceneNode::setScale(float sx, float sy, float sz) {
    scale_ = {sx, sy, sz};
    localDirty_ = true;
    markDirty();
}

void SceneNode::setScale(const Vec3& s) {
    scale_ = s;
    localDirty_ = true;
    markDirty();
}

void SceneNode::setScale2D(float sx, float sy) {
    scale_.x = sx;
    scale_.y = sy;
    localDirty_ = true;
    markDirty();
}

void SceneNode::setScalePartial(const float* values, size_t count) {
    if (count == 0 || !values) return;
    if (count == 1) {
        scale_ = {values[0], values[0], values[0]};
    } else if (count == 2) {
        scale_.x = values[0];
        scale_.y = values[1];
    } else {
        scale_ = {values[0], values[1], values[2]};
    }
    localDirty_ = true;
    markDirty();
}

void SceneNode::addChild(SceneNode* child) {
    if (!child || child == this) return;
    if (child->parent_) child->removeFromParent();
    child->parent_ = this;
    children_.push_back(child);
    child->markDirty();
}

void SceneNode::removeChild(SceneNode* child) {
    if (!child) return;
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
        (*it)->parent_ = nullptr;
        children_.erase(it);
    }
}

void SceneNode::removeFromParent() {
    if (parent_) parent_->removeChild(this);
}

const Mat4& SceneNode::localMatrix() const {
    if (localDirty_) updateLocalMatrix();
    return localMatrix_;
}

const Mat4& SceneNode::worldMatrix() const {
    if (dirty_) updateWorldMatrix();
    return worldMatrix_;
}

Vec3 SceneNode::localToWorld(const Vec3& local) const {
    return bromath::mtransformPoint(worldMatrix(), local);
}

// Build a quaternion from an orthonormal world basis (columns = images of
// local +X/+Y/+Z). Standard matrix->quat conversion (Shepperd's branching).
static Quat quatFromBasis(const Vec3& x, const Vec3& y, const Vec3& z) {
    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return bromath::qnorm(q);
}

void SceneNode::lookAt(const Vec3& worldTarget, const Vec3& up) {
    const Mat4& M = worldMatrix();
    const Vec3 pos{M.at(0, 3), M.at(1, 3), M.at(2, 3)};
    Vec3 fwd = worldTarget - pos;
    if (bromath::vlen2(fwd) < 1e-12f) return;
    fwd = bromath::vnorm(fwd);
    Vec3 right = bromath::vcross(fwd, up);
    if (bromath::vlen2(right) < 1e-12f) {
        // Target along the up axis — pick any perpendicular fallback.
        right = bromath::vcross(fwd, Vec3{1, 0, 0});
        if (bromath::vlen2(right) < 1e-12f) right = bromath::vcross(fwd, Vec3{0, 0, 1});
    }
    right = bromath::vnorm(right);
    const Vec3 upOrtho = bromath::vcross(right, fwd);
    // Camera convention: local -Z looks at the target, so +Z maps to -fwd.
    const Quat qWorld = quatFromBasis(right, upOrtho,
                                      {-fwd.x, -fwd.y, -fwd.z});
    // World -> local: strip the ancestor rotation chain (exact for pure TRS;
    // ancestor non-uniform scale is deliberately ignored — see header).
    Quat qParent{0, 0, 0, 1};
    for (SceneNode* p = parent_; p; p = p->parent_) {
        qParent = bromath::qmul(p->rotation_, qParent);
    }
    setRotation(bromath::qnorm(
        bromath::qmul(bromath::qconjugate(qParent), qWorld)));
}

void SceneNode::traverse(const std::function<void(SceneNode*)>& fn) {
    fn(this);
    for (auto* child : children_) {
        child->traverse(fn);
    }
}

void SceneNode::markDirty() {
    // Always bump the mutation counter, even when already dirty: consumers
    // (shadow-tile cache) compare generations captured at render time, and
    // rendering computes worldMatrix() which clears dirty_ — so the first
    // post-render mutation always lands a bump. Propagation may stop at
    // already-dirty children: a dirty child implies its whole subtree is
    // dirty and none of it was captured since its last bump.
    ++changeGeneration_;
    if (dirty_) return;
    dirty_ = true;
    for (auto* child : children_) {
        child->markDirty();
    }
}

void SceneNode::updateLocalMatrix() const {
    localMatrix_ = bromath::mfromTRS(position_, rotation_, scale_);
    localDirty_ = false;
}

void SceneNode::updateWorldMatrix() const {
    if (localDirty_) updateLocalMatrix();
    if (parent_) {
        worldMatrix_ = bromath::mmul(parent_->worldMatrix(), localMatrix_);
    } else {
        worldMatrix_ = localMatrix_;
    }
    dirty_ = false;
}

} // namespace bro::scene
