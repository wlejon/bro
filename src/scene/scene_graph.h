#pragma once

#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "scene/html_node.h"
#include "scene/agent_binding.h"
#include "scene/ai_world_ticker.h"

#include <glad/gl.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::render { class SkiaRenderer; }
namespace bro::layout { class FontManager; }
namespace brogameagent { class World; }

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
    MeshNode* createMesh(const std::string& name = "");
    HtmlNode* createHtml(const std::string& name = "");

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

    // --- AI integration ---

    /// Attach a brogameagent::World driven at a fixed step by the engine loop.
    /// Caller retains ownership of the world (typically a JS-owned wrapper).
    void attachAIWorld(brogameagent::World* world, float stepHz = 60.0f,
                       int maxStepsPerFrame = 8);
    void detachAIWorld();
    AIWorldTicker* aiTicker() const { return aiTicker_.get(); }

    /// Create or fetch an AgentBinding attached to a node. The binding lives
    /// in a side-map owned by the scene graph so scene_node.h stays AI-free.
    /// Returns nullptr if the node does not belong to this graph.
    AgentBinding* attachAgentBinding(SceneNode* node);
    AgentBinding* agentBinding(uint32_t nodeId) const;
    AgentBinding* agentBinding(SceneNode* node) const;
    void detachAgentBinding(SceneNode* node);

    /// Step the AI world ticker (if attached), then step every agent binding
    /// by dt, syncing agent.x/z/yaw into their scene nodes. Mirrors
    /// syncPhysics() in shape; called once per frame before JS runs.
    void syncAgents(float dt);

    // --- Rendering ---

    /// Set the canvas scene this graph renders into.
    void setCanvasScene(canvas::CanvasScene* scene) { canvasScene_ = scene; }
    canvas::CanvasScene* canvasScene() const { return canvasScene_; }

    /// Set canvas dimensions (needed for FBO sizing).
    void setCanvasSize(int w, int h);
    int canvasWidth() const { return canvasWidth_; }
    int canvasHeight() const { return canvasHeight_; }

    /// Update world matrices for any dirty nodes, then render all visible nodes.
    /// 3D MeshNodes are rendered into an FBO via GL. 2D nodes render via CanvasScene.
    void render();

    /// Get the color texture of the 3D FBO (for compositing). 0 if no 3D content.
    GLuint meshFBOTexture() const { return meshColorTex_; }

    /// Returns true if any MeshNodes were rendered this frame.
    bool hasMeshContent() const { return hasMeshContent_; }

    /// Callback invoked after render() with the current mesh FBO texture (or 0).
    /// Used to push the texture ID to the DOM element for compositing.
    using FBOTextureCallback = std::function<void(unsigned int texId)>;
    void setFBOTextureCallback(FBOTextureCallback cb) { fboTexCb_ = std::move(cb); }

    /// Gizmo overlay provider. Invoked during render() after the mesh +
    /// billboard passes, while the mesh FBO is still bound. Returns a list
    /// of externally-owned MeshNodes (not part of this graph's node table)
    /// to draw as screen-overlay gizmo handles. Drawn with depth-test
    /// disabled so handles always win the depth test — matches DCC tool
    /// convention (handles remain grabbable even when inside geometry).
    using GizmoProvider = std::function<std::vector<MeshNode*>(SceneGraph*)>;
    void setGizmoProvider(GizmoProvider cb) { gizmoProvider_ = std::move(cb); }

    // --- Camera ---

    /// Set a full 3D camera (perspective projection + lookAt view).
    /// Call with fovY in radians, aspect ratio, near/far clip, eye position, look-at target.
    void setCamera(float fovY, float aspect, float nearZ, float farZ,
                   const Vec3& eye, const Vec3& target, const Vec3& up = {0, 1, 0});

    /// Set a 3D camera from a quaternion orientation (no lookAt — avoids precision loss).
    /// The quaternion represents the camera's world-space orientation.
    void setCameraQuat(float fovY, float aspect, float nearZ, float farZ,
                       const Vec3& eye, const Quat& orientation);

    /// Set an orthographic camera.
    void setCameraOrtho(float left, float right, float bottom, float top,
                        float nearZ, float farZ,
                        const Vec3& eye, const Vec3& target, const Vec3& up = {0, 1, 0});

    /// Direct matrix access (for Phase 4 MeshNode rendering).
    const Mat4& viewMatrix() const { return viewMatrix_; }
    const Mat4& projectionMatrix() const { return projectionMatrix_; }

    /// Camera eye position (for lighting calculations).
    const Vec3& cameraEye() const { return cameraEye_; }

    /// Unproject canvas-local pixel coordinates to a world-space ray.
    /// `localX` / `localY` are in pixels relative to the canvas (top-left
    /// origin). Returns true on success; false if the camera has not been
    /// initialised. Uses the current perspective projection's tan(fovY/2)
    /// and view matrix orientation to build the ray, so it works for both
    /// setCamera() and setCameraQuat() code paths.
    bool unprojectLocal(float localX, float localY,
                        Vec3& outOrigin, Vec3& outDir) const;

    /// Set distance fog parameters. start/end in world units, color is RGB [0,1].
    void setFog(float start, float end, float r, float g, float b) {
        fogStart_ = start; fogEnd_ = end;
        fogColor_[0] = r; fogColor_[1] = g; fogColor_[2] = b;
    }

    // --- Legacy 2D camera (sets ortho projection + top-down view) ---
    void setCameraPosition(float x, float y);
    void setCameraZoom(float z);
    float cameraX() const { return cameraX_; }
    float cameraY() const { return cameraY_; }
    float cameraZoom() const { return cameraZoom_; }

    /// Iterate all HtmlNodes and run dirty layout/paint/GL upload. Runs on
    /// the main/GL thread before scene render so the detached Documents stay
    /// serialized with JS mutations.
    void materializeHtmlNodes(render::SkiaRenderer* renderer,
                              layout::FontManager* fontManager);

    /// True if any HtmlNode's DOM subtree is dirty. Read on main thread to
    /// force a frame when only scene-graph HTML content has changed.
    bool hasPendingHtmlWork() const;

private:
    void renderNode(SceneNode* node);
    void renderMeshNode(MeshNode* mesh);
    void renderBillboardNode(SceneNode* node);
    void collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids);

    // --- Mesh GL pipeline (lazy init) ---
    void ensureMeshPipeline();
    void ensureMeshFBO();
    void destroyMeshFBO();

    // --- Billboard GL pipeline (lazy init) ---
    void ensureBillboardPipeline();

    std::unique_ptr<SceneNode> root_;
    std::unordered_map<uint32_t, std::unique_ptr<SceneNode>> nodes_;

    // AI integration
    std::unique_ptr<AIWorldTicker> aiTicker_;
    std::unordered_map<uint32_t, std::unique_ptr<AgentBinding>> agentBindings_;

    canvas::CanvasScene* canvasScene_ = nullptr;
    physics::PhysicsWorld* physicsWorld_ = nullptr;

    // 3D camera matrices
    Mat4 viewMatrix_;
    Mat4 projectionMatrix_;
    Vec3 cameraEye_;

    // Legacy 2D camera state (drives the CanvasScene 2D path)
    float cameraX_ = 0, cameraY_ = 0;
    float cameraZoom_ = 1.0f;

    // Canvas size for FBO
    int canvasWidth_ = 0, canvasHeight_ = 0;

    // Mesh rendering GL resources (shared across all MeshNodes)
    GLuint meshProgram_ = 0;
    GLint uMVP_ = -1;
    GLint uModel_ = -1;
    GLint uColor_ = -1;
    GLint uLightDir_ = -1;
    GLint uCameraPos_ = -1;
    GLint uEmissive_ = -1;
    GLint uUseVertexColor_ = -1;
    GLint uUseTexture_ = -1;
    GLint uBaseColorTex_ = -1;
    GLint uFogStart_ = -1;
    GLint uFogEnd_ = -1;
    GLint uFogColor_ = -1;
    GLint uNearClip_ = -1;

    // Mesh FBO
    GLuint meshFBO_ = 0;
    GLuint meshColorTex_ = 0;
    GLuint meshDepthRBO_ = 0;
    int meshFBOWidth_ = 0, meshFBOHeight_ = 0;

    bool hasMeshContent_ = false;
    FBOTextureCallback fboTexCb_;
    GizmoProvider gizmoProvider_;

    // Distance fog
    float fogStart_ = 0.0f;
    float fogEnd_ = 0.0f;
    float fogColor_[3] = {0.0f, 0.0f, 0.0f};

    // --- Billboard pipeline (lazy init) ---
    GLuint bbProgram_ = 0;
    GLuint bbVAO_ = 0;
    GLuint bbVBO_ = 0;
    GLint bbUVP_ = -1;
    GLint bbUAnchorRel_ = -1;
    GLint bbURight_ = -1;
    GLint bbUUp_ = -1;
    GLint bbUHalfSize_ = -1;
    GLint bbUShapeMode_ = -1;
    GLint bbUColor_ = -1;
    GLint bbUStroke_ = -1;
    GLint bbUStrokeWidth_ = -1;
    GLint bbUTex_ = -1;
};

} // namespace bro::scene
