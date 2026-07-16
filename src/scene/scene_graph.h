#pragma once

#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/gaussian_splat_node.h"
#include "scene/html_node.h"
#include "scene/light_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/agent_binding.h"
#include "scene/ai_world_ticker.h"
#include "scene/scene_renderer.h"
#include "scene/tween.h"

#include <glad/gl.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::render { class SkiaRenderer; }
namespace brogameagent { class World; }

namespace bro::scene {

/// Per-canvas scene graph. Owns all nodes and manages update/render traversal.
/// GL rendering (pipelines, FBOs, shadows, IBL, post stack) lives in the
/// SceneRenderer this graph owns; the render-config API below forwards to it.
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
    SkinnedMeshNode* createSkinnedMesh(const std::string& name = "");
    InstancedMeshNode* createInstancedMesh(const std::string& name = "");
    GaussianSplatNode* createGaussianSplat(const std::string& name = "");
    HtmlNode* createHtml(const std::string& name = "");
    LightNode* createLight(const std::string& name = "");
    ParticleNode* createParticles(const std::string& name = "");
    Particles3DNode* createParticles3D(const std::string& name = "");

    /// Destroy a node and remove it from the tree. Also destroys children.
    void destroyNode(SceneNode* node);

    /// Find a node by ID.
    SceneNode* findById(uint32_t id) const;

    /// Find a node by name (first match).
    SceneNode* findByName(const std::string& name) const;

    // --- Tweens ---

    /// Create a property tween (owned by this graph, ticked from
    /// tickAnimations after node ticks). It persists until destroyTween —
    /// finished tweens can be restarted with start().
    Tween* createTween();

    /// Resolve a tween by id. Returns nullptr for unknown or destroyed ids.
    Tween* findTween(uint32_t id) const;

    /// Destroy a tween. Deferred-safe: callable from the tween's own
    /// callbacks (the entry is hidden immediately and erased after the
    /// current tick pass).
    void destroyTween(uint32_t id);

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

    /// Tick all node animations / particle simulation / etc. Called once per
    /// engine frame (before JS callbacks fire and before render()). Walks the
    /// node table and calls SceneNode::onTick(dt) on each entry, including the
    /// root and invisible nodes — invisible particles still need to expire.
    /// Headless drives this with the virtual-time step so screenshot tests
    /// of effects are deterministic.
    void tickAnimations(float dtSec);

    /// Update world matrices for any dirty nodes, then render all visible nodes.
    /// 3D MeshNodes are rendered into an FBO via GL. 2D nodes render via CanvasScene.
    void render();

    /// Get the color texture of the 3D FBO (for compositing). 0 if no 3D content.
    GLuint meshFBOTexture() const { return renderer_.meshFBOTexture(); }

    /// Returns true if any MeshNodes were rendered this frame.
    bool hasMeshContent() const { return renderer_.hasMeshContent(); }

    /// Read RGBA8 pixels from the post-tonemap LDR FBO. Used by offscreen
    /// capture (artstation defineScene): renders 3D content with alpha=0 in
    /// uncovered regions, so the readback is suitable for compositing into a
    /// 2D canvas cell via putImageData. Pixels are returned in top-down row
    /// order (matches CSS / ImageData), unlike GL's bottom-up native layout.
    /// Returns empty vector if the tonemap FBO hasn't been populated yet
    /// (e.g. no render() with 3D content has run). Must be called on the GL
    /// thread.
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH) {
        return renderer_.readTonemapPixelsRGBA(outW, outH);
    }

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
                   const bromath::Vec3& eye, const bromath::Vec3& target, const bromath::Vec3& up = {0, 1, 0});

    /// When true, setCanvasSize() rebuilds the projection matrix using the
    /// new canvas aspect ratio (and the stored fovY/near/far). Use this when
    /// the caller wants the camera aspect to track the viewport; pin an
    /// explicit aspect in setCamera() and leave this false for fixed-aspect
    /// / cinematic cameras.
    void setCameraAspectFollowsCanvas(bool on) { cameraAspectFollowsCanvas_ = on; }

    /// Set a 3D camera from a quaternion orientation (no lookAt — avoids precision loss).
    /// The quaternion represents the camera's world-space orientation.
    void setCameraQuat(float fovY, float aspect, float nearZ, float farZ,
                       const bromath::Vec3& eye, const bromath::Quat& orientation);

    /// Set an orthographic camera.
    void setCameraOrtho(float left, float right, float bottom, float top,
                        float nearZ, float farZ,
                        const bromath::Vec3& eye, const bromath::Vec3& target, const bromath::Vec3& up = {0, 1, 0});

    /// Direct matrix access (for MeshNode rendering).
    const bromath::Mat4& viewMatrix() const { return viewMatrix_; }
    const bromath::Mat4& projectionMatrix() const { return projectionMatrix_; }

    /// Camera eye position (for lighting calculations).
    const bromath::Vec3& cameraEye() const { return cameraEye_; }

    /// Unproject canvas-local pixel coordinates to a world-space ray.
    /// `localX` / `localY` are in pixels relative to the canvas (top-left
    /// origin). Returns true on success; false if the camera has not been
    /// initialised. Uses the current perspective projection's tan(fovY/2)
    /// and view matrix orientation to build the ray, so it works for both
    /// setCamera() and setCameraQuat() code paths.
    bool unprojectLocal(float localX, float localY,
                        bromath::Vec3& outOrigin, bromath::Vec3& outDir) const;

    /// Pick the front-most world-anchored HtmlNode hit by a canvas-local
    /// ray. On hit, writes the HtmlNode pointer and the local CSS-pixel
    /// coordinates inside its layout box (top-left origin, matching the
    /// node's raster surface). Returns false if no node is hit, the camera
    /// is uninitialised, or there are no HtmlNodes in the graph.
    struct HtmlNodePick {
        HtmlNode* node = nullptr;
        float localPxX = 0.0f;
        float localPxY = 0.0f;
        float distance = 0.0f;
    };
    bool pickHtmlNode(float canvasLocalX, float canvasLocalY,
                      HtmlNodePick& out) const;

    // --- Render settings (forwarded to the SceneRenderer) ---

    /// Set distance fog parameters. start/end in world units, color is RGB [0,1].
    void setFog(float start, float end, float r, float g, float b) {
        renderer_.setFog(start, end, r, g, b);
    }

    /// Tone mapping mode applied when composing the HDR mesh FBO to the
    /// caller-facing LDR texture. ACES matches modern filmic defaults;
    /// Reinhard is a cheaper fallback; Linear is raw clamp to 0-1.
    using ToneMap = SceneRenderer::ToneMap;

    /// Configure tonemap + exposure. Exposure is a pre-tonemap multiplier
    /// (1.0 = neutral, 2.0 = +1 stop). `gamma` is the post-tonemap output
    /// gamma; pass 1.0 to leave linear output (default 2.2 for sRGB).
    void setToneMap(ToneMap mode, float exposure = 1.0f, float gamma = 2.2f) {
        renderer_.setToneMap(mode, exposure, gamma);
    }
    ToneMap toneMap() const { return renderer_.toneMap(); }
    float exposure() const { return renderer_.exposure(); }

    /// Ambient fill added to every fragment after per-light contributions.
    /// Used as the flat fallback when IBL is disabled (IBL replaces it with
    /// split-sum env lighting when enabled). RGB in linear 0-1.
    void setAmbient(float r, float g, float b) { renderer_.setAmbient(r, g, b); }

    /// Screen-space tilt-shift depth-of-field, applied after tonemap on the
    /// LDR result. A horizontal band stays sharp; the scene blurs toward the
    /// top and bottom edges, the cue that reads as "miniature". Off by default
    /// (enabled=false leaves the pipeline untouched).
    ///   enabled      — toggle the whole pass.
    ///   focusCenter  — band center in [0,1] vertical screen space (0.5 = mid).
    ///   focusWidth   — half-height of the fully-sharp band (UV units).
    ///   feather      — blur ramp distance past the band edge (UV units).
    ///   strength     — blur radius multiplier (texels at half-res).
    ///   saturation   — chroma boost for the toy look (1.0 = unchanged).
    ///   contrast     — contrast boost (1.0 = unchanged).
    void setTiltShift(bool enabled, float focusCenter, float focusWidth,
                      float feather, float strength, float saturation,
                      float contrast) {
        renderer_.setTiltShift(enabled, focusCenter, focusWidth, feather,
                               strength, saturation, contrast);
    }
    bool tiltShiftEnabled() const { return renderer_.tiltShiftEnabled(); }

    /// HDR bloom: bright areas (luminance above `threshold`) bleed a soft glow,
    /// added back in HDR before tonemap so highlights bloom filmically. Off by
    /// default (intensity 0 leaves the tonemap pass unchanged).
    ///   enabled   — toggle the bright-pass + blur.
    ///   threshold — luminance above which a fragment contributes (HDR, ~1.0+).
    ///   intensity — additive scale of the blurred bloom in HDR.
    ///   strength  — blur radius multiplier (texels at half-res).
    void setBloom(bool enabled, float threshold, float intensity, float strength) {
        renderer_.setBloom(enabled, threshold, intensity, strength);
    }
    bool bloomEnabled() const { return renderer_.bloomEnabled(); }

    /// Wind sway parameters consumed by the mesh vertex shader. Per-vertex
    /// `windBend` (vertex color R, 0..1) modulates the global displacement
    /// `windDir * sin(windTime*windFreq + dot(pos.xz, k)) * strength * bend`.
    void setWind(float dirX, float dirY, float dirZ,
                 float strength, float frequency) {
        renderer_.setWind(dirX, dirY, dirZ, strength, frequency);
    }
    /// Advance the wind clock by `dt` seconds. Engine drives this with the
    /// frame's virtual delta so offline renders stay deterministic.
    void advanceWindTime(float dt) { renderer_.advanceWindTime(dt); }
    void resetWindTime() { renderer_.resetWindTime(); }
    float windTime() const { return renderer_.windTime(); }

    // --- Shadow mapping ---

    /// Configure shadow atlas and quality. `atlasSize` is the side length of
    /// the square depth texture (e.g. 4096). `pcfTaps` is 1 (single sample)
    /// or 3 (3x3 PCF, default). Defaults are sane — call only to tune.
    void setShadowQuality(int atlasSize, int pcfTaps) {
        renderer_.setShadowQuality(atlasSize, pcfTaps);
    }
    int shadowAtlasSize() const { return renderer_.shadowAtlasSize(); }
    int shadowPCFTaps() const { return renderer_.shadowPCFTaps(); }

    /// Editor affordance: when true, every LightNode renders a small
    /// kind-specific marker billboard at its world position (visible in
    /// the 3D FBO, depth-tested against geometry). Also makes lights
    /// pickable via `raycast()`: hits return the LightNode as `hit.node`.
    void setShowLightIcons(bool on) { renderer_.setShowLightIcons(on); }
    bool showLightIcons() const { return renderer_.showLightIcons(); }

    // --- IBL environment ---

    /// Load an HDR equirectangular image (.hdr) and convert it to a 512²
    /// cubemap that backs both skybox rendering and IBL precompute
    /// (irradiance + prefilter, added in later passes). Returns true on
    /// success; on failure the previous environment is kept. Pass an empty
    /// path to clear. Must be called on the GL thread (JS bindings already
    /// satisfy this).
    bool loadEnvironment(const std::string& hdrPath) {
        return renderer_.loadEnvironment(hdrPath);
    }
    void clearEnvironment() { renderer_.clearEnvironment(); }
    bool hasEnvironment() const { return renderer_.hasEnvironment(); }
    const std::string& environmentPath() const { return renderer_.environmentPath(); }

    /// Multiplier applied to all IBL contributions (skybox + irradiance +
    /// prefilter). 1.0 = neutral. Tune independently of sun intensity.
    void  setEnvironmentIntensity(float i) { renderer_.setEnvironmentIntensity(i); }
    float environmentIntensity() const { return renderer_.environmentIntensity(); }

    /// Y-axis rotation of the environment in radians. Useful for aligning
    /// the visible sun in the HDR with the engine's directional sun light.
    void  setEnvironmentRotation(float r) { renderer_.setEnvironmentRotation(r); }
    float environmentRotation() const { return renderer_.environmentRotation(); }

    // --- Legacy 2D camera (sets ortho projection + top-down view) ---
    void setCameraPosition(float x, float y);
    void setCameraZoom(float z);
    float cameraX() const { return cameraX_; }
    float cameraY() const { return cameraY_; }
    float cameraZoom() const { return cameraZoom_; }

    /// Iterate all HtmlNodes and run dirty layout/paint/GL upload. Runs on
    /// the main/GL thread before scene render so the detached Documents stay
    /// serialized with JS mutations.
    void materializeHtmlNodes(render::SkiaRenderer* renderer);

    /// True if any HtmlNode's DOM subtree is dirty. Read on main thread to
    /// force a frame when only scene-graph HTML content has changed.
    bool hasPendingHtmlWork() const;

private:
    // The renderer walks nodes/camera state directly through its graph
    // back-reference; it is the only class with private access.
    friend class SceneRenderer;

    void collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids);

    std::unique_ptr<SceneNode> root_;
    std::unordered_map<uint32_t, std::unique_ptr<SceneNode>> nodes_;

    // AI integration
    std::unique_ptr<AIWorldTicker> aiTicker_;
    std::unordered_map<uint32_t, std::unique_ptr<AgentBinding>> agentBindings_;

    // Tweens (ticked from tickAnimations; destroyed entries are swept there)
    std::unordered_map<uint32_t, std::unique_ptr<Tween>> tweens_;
    uint32_t nextTweenId_ = 1;

    canvas::CanvasScene* canvasScene_ = nullptr;
    physics::PhysicsWorld* physicsWorld_ = nullptr;

    // 3D camera matrices
    bromath::Mat4 viewMatrix_;
    bromath::Mat4 projectionMatrix_;
    bromath::Vec3 cameraEye_;

    // Cached camera intrinsics — needed by CSM cascade fitting (which has to
    // walk the view-frustum corners). Set by every setCamera*() entry point.
    float cameraNearZ_ = 0.1f;
    float cameraFarZ_  = 100.0f;
    float cameraFovY_  = 1.0f;
    float cameraAspect_ = 1.0f;
    bool  cameraIsPerspective_ = true;
    // When the caller didn't pin an explicit aspect (e.g. omitted `aspect`
    // in scene.setCamera), the projection matrix must stay in lock-step with
    // canvas/FBO dimensions — otherwise resizing the window squishes content
    // because the FBO grows while the projection stays baked at the old
    // aspect. setCanvasSize() rebuilds the projection when this is true.
    bool  cameraAspectFollowsCanvas_ = false;

    // Legacy 2D camera state (drives the CanvasScene 2D path)
    float cameraX_ = 0, cameraY_ = 0;
    float cameraZoom_ = 1.0f;

    // Canvas size for FBO
    int canvasWidth_ = 0, canvasHeight_ = 0;

    FBOTextureCallback fboTexCb_;
    GizmoProvider gizmoProvider_;

    // GL rendering: pipelines, FBOs, shadows, IBL, post stack. The renderer
    // never touches graph state in its destructor, so member order is not
    // load-bearing; it lives last simply to keep the hot node/camera state
    // at stable offsets.
    SceneRenderer renderer_{*this};
};

} // namespace bro::scene
