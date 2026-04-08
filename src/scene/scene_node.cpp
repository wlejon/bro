#include "scene/scene_node.h"
#include <algorithm>

namespace bro::scene {

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

void SceneNode::setRotation(float radians) {
    rotation_ = radians;
    localDirty_ = true;
    markDirty();
}

void SceneNode::setScale(float sx, float sy) {
    scale_ = {sx, sy, 1};
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

const Mat3& SceneNode::localMatrix() const {
    if (localDirty_) updateLocalMatrix();
    return localMatrix_;
}

const Mat3& SceneNode::worldMatrix() const {
    if (dirty_) updateWorldMatrix();
    return worldMatrix_;
}

Vec3 SceneNode::localToWorld(const Vec3& local) const {
    return worldMatrix().transformPoint(local);
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
    // T * R * S
    localMatrix_ = Mat3::translate(position_.x, position_.y)
                  * Mat3::rotate(rotation_)
                  * Mat3::scale(scale_.x, scale_.y);
    localDirty_ = false;
}

void SceneNode::updateWorldMatrix() const {
    if (localDirty_) updateLocalMatrix();
    if (parent_) {
        worldMatrix_ = parent_->worldMatrix() * localMatrix_;
    } else {
        worldMatrix_ = localMatrix_;
    }
    dirty_ = false;
}

} // namespace bro::scene
