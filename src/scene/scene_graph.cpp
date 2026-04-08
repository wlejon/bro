#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"
#include "physics/physics_world.h"

namespace bro::scene {

SceneGraph::SceneGraph() {
    root_ = std::make_unique<SceneNode>("__root__");
}

SceneGraph::~SceneGraph() {
    // Clear children before destroying nodes map, since node destructors
    // detach from parent (which accesses siblings vector).
    // Destroy in reverse creation order to handle parent-child deps.
    root_->traverse([](SceneNode* n) {
        // Orphan all children to prevent dangling parent pointers
        for (auto* c : n->children()) {
            // Will be destroyed by the nodes_ map
        }
    });
    nodes_.clear();
}

SceneNode* SceneGraph::createNode(const std::string& name) {
    auto node = std::make_unique<SceneNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

ShapeNode* SceneGraph::createShape(const std::string& name) {
    auto node = std::make_unique<ShapeNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

SpriteNode* SceneGraph::createSprite(const std::string& name) {
    auto node = std::make_unique<SpriteNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

PhysicsNode* SceneGraph::createPhysicsNode(const std::string& name) {
    auto node = std::make_unique<PhysicsNode>(name);
    auto* ptr = node.get();
    nodes_[ptr->id()] = std::move(node);
    return ptr;
}

void SceneGraph::collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids) {
    for (auto* child : node->children()) {
        collectDestroyList(child, ids);
    }
    ids.push_back(node->id());
}

void SceneGraph::destroyNode(SceneNode* node) {
    if (!node || node == root_.get()) return;

    // Collect this node and all descendants
    std::vector<uint32_t> ids;
    collectDestroyList(node, ids);

    // Detach from parent first
    node->removeFromParent();

    // Destroy all collected nodes (children first due to ordering)
    for (auto id : ids) {
        nodes_.erase(id);
    }
}

SceneNode* SceneGraph::findById(uint32_t id) const {
    auto it = nodes_.find(id);
    return (it != nodes_.end()) ? it->second.get() : nullptr;
}

SceneNode* SceneGraph::findByName(const std::string& name) const {
    for (auto& [id, node] : nodes_) {
        if (node->name() == name) return node.get();
    }
    return nullptr;
}

void SceneGraph::setCamera(float fovY, float aspect, float nearZ, float farZ,
                           const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::perspective(fovY, aspect, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
}

void SceneGraph::setCameraOrtho(float left, float right, float bottom, float top,
                                float nearZ, float farZ,
                                const Vec3& eye, const Vec3& target, const Vec3& up) {
    projectionMatrix_ = Mat4::orthographic(left, right, bottom, top, nearZ, farZ);
    viewMatrix_ = Mat4::lookAt(eye, target, up);
    cameraEye_ = eye;
}

void SceneGraph::setCameraPosition(float x, float y) {
    cameraX_ = x;
    cameraY_ = y;
}

void SceneGraph::setCameraZoom(float z) {
    cameraZoom_ = z;
}

void SceneGraph::syncPhysics() {
    if (!physicsWorld_) return;
    for (auto& [id, node] : nodes_) {
        if (node->type() == SceneNode::Type::Physics) {
            auto* pn = static_cast<PhysicsNode*>(node.get());
            if (pn->autoSync()) {
                pn->syncFromPhysics(physicsWorld_);
            }
        }
    }
}

void SceneGraph::render() {
    if (!canvasScene_) return;

    // Clear canvas before redrawing the scene
    canvasScene_->reset();

    // Apply camera transform
    canvasScene_->save();
    canvasScene_->translate(-cameraX_, -cameraY_);
    if (cameraZoom_ != 1.0f) {
        canvasScene_->scale(cameraZoom_, cameraZoom_);
    }

    // Depth-first render traversal
    renderNode(root_.get());

    canvasScene_->restore();
}

void SceneGraph::renderNode(SceneNode* node) {
    if (!node->visible()) return;
    node->onRender(*this);
    for (auto* child : node->children()) {
        renderNode(child);
    }
}

} // namespace bro::scene
