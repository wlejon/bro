#include "scene/scene_node.h"
#include <algorithm>

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

void SceneNode::traverse(const std::function<void(SceneNode*)>& fn) {
    fn(this);
    for (auto* child : children_) {
        child->traverse(fn);
    }
}

void SceneNode::markDirty() {
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
