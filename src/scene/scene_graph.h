#pragma once

#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::physics { class PhysicsWorld; }

namespace bro::scene {

/// Per-canvas scene graph. Owns all nodes and manages update/render traversal.
class SceneGraph {
public:
    SceneGraph();
    ~SceneGraph();

    /// The root node. All scene content is added as children of the root.
    SceneNode* root() { return root_.get(); }

    // --- Node factory (scene graph owns all nodes) ---

    SceneNode* createNode(const std::string& name = "");
    ShapeNode* createShape(const std::string& name = "");
    SpriteNode* createSprite(const std::string& name = "");
    PhysicsNode* createPhysicsNode(const std::string& name = "");

    /// Destroy a node and remove it from the tree. Also destroys children.
    void destroyNode(SceneNode* node);

    /// Find a node by ID.
    SceneNode* findById(uint32_t id) const;

    /// Find a node by name (first match).
    SceneNode* findByName(const std::string& name) const;

    // --- Physics integration ---

    void setPhysicsWorld(physics::PhysicsWorld* world) { physicsWorld_ = world; }
    physics::PhysicsWorld* physicsWorld() const { return physicsWorld_; }

    /// Sync physics body transforms → scene node transforms.
    /// Call after physics step completes (when physics thread is idle).
    void syncPhysics();

    // --- Rendering ---

    /// Set the canvas scene this graph renders into.
    void setCanvasScene(canvas::CanvasScene* scene) { canvasScene_ = scene; }
    canvas::CanvasScene* canvasScene() const { return canvasScene_; }

    /// Update world matrices for any dirty nodes, then render all visible nodes.
    void render();

    // --- Camera ---

    /// Set a full 3D camera (perspective projection + lookAt view).
    /// Call with fovY in radians, aspect ratio, near/far clip, eye position, look-at target.
    void setCamera(float fovY, float aspect, float nearZ, float farZ,
                   const Vec3& eye, const Vec3& target, const Vec3& up = {0, 1, 0});

    /// Set an orthographic camera.
    void setCameraOrtho(float left, float right, float bottom, float top,
                        float nearZ, float farZ,
                        const Vec3& eye, const Vec3& target, const Vec3& up = {0, 1, 0});

    /// Direct matrix access (for Phase 4 MeshNode rendering).
    const Mat4& viewMatrix() const { return viewMatrix_; }
    const Mat4& projectionMatrix() const { return projectionMatrix_; }

    /// Camera eye position (for lighting calculations).
    const Vec3& cameraEye() const { return cameraEye_; }

    // --- Legacy 2D camera (sets ortho projection + top-down view) ---
    void setCameraPosition(float x, float y);
    void setCameraZoom(float z);
    float cameraX() const { return cameraX_; }
    float cameraY() const { return cameraY_; }
    float cameraZoom() const { return cameraZoom_; }

private:
    void renderNode(SceneNode* node);
    void collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids);

    std::unique_ptr<SceneNode> root_;
    std::unordered_map<uint32_t, std::unique_ptr<SceneNode>> nodes_;

    canvas::CanvasScene* canvasScene_ = nullptr;
    physics::PhysicsWorld* physicsWorld_ = nullptr;

    // 3D camera matrices
    Mat4 viewMatrix_;
    Mat4 projectionMatrix_;
    Vec3 cameraEye_;

    // Legacy 2D camera state (drives the CanvasScene 2D path)
    float cameraX_ = 0, cameraY_ = 0;
    float cameraZoom_ = 1.0f;
};

} // namespace bro::scene
