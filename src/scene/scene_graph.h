#pragma once

#include "scene/scene_node.h"
#include "scene/camera_node.h"
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
    CameraNode* createCamera(const std::string& name = "");

    /// Destroy a node and remove it from the tree. Also destroys children.
    void destroyNode(SceneNode* node);

    /// Find a node by ID.
    SceneNode* findById(uint32_t id) const;

    /// Resolve a node id to a live node, or nullptr once the node has been
    /// destroyed (directly, via an ancestor subtree destroy, or graph
    /// teardown). Unlike findById this also resolves the root node, which
    /// lives outside the id table. SceneNode ids come from a process-wide
    /// monotonic counter and are never reused, so a stale id can never alias
    /// a different node — this is the liveness check JS node wrappers resolve
    /// through on every call.
    SceneNode* resolveNode(uint32_t id) const;

    /// Shared liveness token for JS-side wrappers (SceneGraph / SceneNode /
    /// Tween). Wrappers hold only a weak_ptr and re-resolve on every call: a
    /// failed lock() or a null `graph` means this SceneGraph was destroyed
    /// (canvas detached and pruned, or engine teardown) and the wrapper must
    /// no-op instead of touching freed memory. Same pattern as
    /// OutputTextureSource below, kept separate so texture-handle and
    /// JS-wrapper lifetimes stay independently documented. Created in the
    /// constructor; `graph` is nulled first thing in ~SceneGraph.
    struct LivenessToken { SceneGraph* graph = nullptr; };
    const std::shared_ptr<LivenessToken>& livenessToken() const { return liveToken_; }

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
    /// `keepAlive` is an opaque token held for the duration of the attachment
    /// — the JS layer passes a holder that pins the world's JS wrapper so the
    /// raw pointer can't dangle if the app drops its own reference. Released
    /// on detachAIWorld() / ~SceneGraph (both run before JS runtime teardown).
    void attachAIWorld(brogameagent::World* world, float stepHz = 60.0f,
                       int maxStepsPerFrame = 8,
                       std::shared_ptr<void> keepAlive = {});
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

    /// Current LDR output texture of this scene's 3D pipeline — the same
    /// texture the compositor samples (the tilt-shift output when that pass
    /// ran, else the tonemap output). 0 until the first 3D render. The id is
    /// NOT stable across frames: the FBO chain is recreated on canvas resize
    /// and renderScale changes, so consumers must re-resolve every frame
    /// (see outputTextureSource()).
    unsigned outputColorTexture() const { return renderer_.finalColorTex(); }

    /// Shared liveness token for scene-as-texture consumers (a mesh in
    /// another scene sampling this scene's output). A consumer keeps a
    /// weak_ptr to this token and resolves it at draw time: a failed lock()
    /// or a null `graph` means this SceneGraph is gone and the consumer must
    /// stop sampling (MeshNode falls back to its plain base color).
    /// Deliberately weak so a handle can never extend the source scene's
    /// lifetime. Created lazily on first request; invalidated in ~SceneGraph.
    struct OutputTextureSource { SceneGraph* graph = nullptr; };
    std::shared_ptr<OutputTextureSource> outputTextureSource();

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

    // --- Camera nodes (see camera_node.h for the full contract) ---

    /// Activate a camera node: while active, the view/projection are derived
    /// from its world transform + projection params every tick (after
    /// animations/tweens) and at the top of render(). Precedence is
    /// last-call-wins: any imperative setCamera*() call deactivates the
    /// node. Pass nullptr to deactivate explicitly (the last derived view
    /// is kept until the next camera call). A camera from another graph is
    /// ignored.
    void setActiveCamera(CameraNode* cam);

    /// The active camera node, or nullptr when the imperative view is in
    /// effect (or the active node was destroyed).
    CameraNode* activeCamera() const;

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

    /// Exponential-squared + height fog (overrides the linear ramp while
    /// density > 0). See SceneRenderer::setFogExp.
    void setFogExp(float density, float heightFalloff, float startDistance) {
        renderer_.setFogExp(density, heightFalloff, startDistance);
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

    /// Screen-space ambient occlusion, multiplied into the lit HDR image
    /// before tonemap. See SceneRenderer::setSSAO for parameter semantics.
    void setSSAO(bool enabled, float radius, float intensity, float bias) {
        renderer_.setSSAO(enabled, radius, intensity, bias);
    }
    bool ssaoEnabled() const { return renderer_.ssaoEnabled(); }

    /// Depth-based depth-of-field on the HDR image before bloom + tonemap.
    /// See SceneRenderer::setDepthOfField for parameter semantics.
    void setDepthOfField(bool enabled, float focusDistance, float focusRange,
                         float maxBlur) {
        renderer_.setDepthOfField(enabled, focusDistance, focusRange, maxBlur);
    }
    bool depthOfFieldEnabled() const { return renderer_.depthOfFieldEnabled(); }

    /// 3D color-grading LUT applied after tonemapping. See
    /// SceneRenderer::loadColorLUT for the strip-image contract.
    bool loadColorLUT(const std::string& path, int size, float amount) {
        return renderer_.loadColorLUT(path, size, amount);
    }
    void clearColorLUT() { renderer_.clearColorLUT(); }
    bool hasColorLUT() const { return renderer_.hasColorLUT(); }

    /// FXAA on the final LDR image (always the last post pass). Complements
    /// MSAA — see SceneRenderer::setFXAA.
    void setFXAA(bool enabled) { renderer_.setFXAA(enabled); }
    bool fxaaEnabled() const { return renderer_.fxaaEnabled(); }

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

    /// Static shadow-tile cache (default on): atlas tiles whose light
    /// projection and overlapping caster set are unchanged are reused
    /// instead of re-rendered. Strictly conservative — pixels are identical
    /// either way — so the toggle exists for debugging and bisecting.
    /// See SceneRenderer::setShadowCache.
    void setShadowCache(bool on) { renderer_.setShadowCache(on); }
    bool shadowCache() const { return renderer_.shadowCacheEnabled(); }

    /// Editor affordance: when true, every LightNode renders a small
    /// kind-specific marker billboard at its world position (visible in
    /// the 3D FBO, depth-tested against geometry). Also makes lights
    /// pickable via `raycast()`: hits return the LightNode as `hit.node`.
    void setShowLightIcons(bool on) { renderer_.setShowLightIcons(on); }
    bool showLightIcons() const { return renderer_.showLightIcons(); }

    // --- Frustum culling ---

    /// Toggle frustum culling for the forward + shadow passes (default on).
    /// Culling is strictly conservative — output pixels are identical either
    /// way — so this exists for debugging and regression bisecting.
    void setFrustumCulling(bool on) { renderer_.setFrustumCulling(on); }
    bool frustumCulling() const { return renderer_.frustumCullingEnabled(); }

    /// Per-category drawn/culled counters from the most recent render().
    const CullStats& cullStats() const { return renderer_.cullStats(); }

    // --- Render-target quality ---

    /// Internal render-resolution scale (clamped 0.25-2.0, default 1.0).
    /// Multiplies the 3D render-target sizes (HDR mesh FBO + tonemap +
    /// bloom + tilt-shift chains); the compositor samples the result at the
    /// CSS element box, so layout, picking and camera aspect are unaffected.
    /// <1 trades sharpness for fill-rate, >1 supersamples.
    void  setRenderScale(float s) { renderer_.setRenderScale(s); }
    float renderScale() const { return renderer_.renderScale(); }

    /// MSAA sample count for the HDR 3D passes (0/1 = off; clamped to the
    /// driver's GL_MAX_SAMPLES at allocation). Multisampled color + depth
    /// resolve into the single-sampled targets before tonemap, so the post
    /// stack, unlit overlay and soft particles are unaffected.
    void setMSAA(int samples) { renderer_.setMSAA(samples); }
    int  msaa() const { return renderer_.msaaSamples(); }

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

    // --- Custom mesh shaders ---

    /// Eagerly compile + cache a mesh-program variant for a pair of user
    /// GLSL chunks (mesh.setShader validation path). `target` picks the
    /// pipeline flavour (static / skinned / instanced) — skinned nodes
    /// compile Static AND Skinned since a not-ready skin degrades to the
    /// static path. True when linked (or already cached); false with the
    /// full driver log in errOut. Must be called on the GL thread (JS
    /// bindings already satisfy this). `key` is CustomShaderState's cache
    /// key: vertex + '\x1f' + fragment. Non-empty vertex chunks also
    /// pre-compile the matching shadow variant (failure there only warns —
    /// the caster falls back to the undisplaced default silhouette).
    bool compileCustomShader(SceneRenderer::CustomShaderTarget target,
                             const std::string& key,
                             const std::string& vertexChunk,
                             const std::string& fragmentChunk,
                             std::string& errOut) {
        return renderer_.compileCustomShader(target, key, vertexChunk,
                                             fragmentChunk, errOut);
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
    void materializeHtmlNodes(render::SkiaRenderer* renderer);

    /// True if any HtmlNode's DOM subtree is dirty. Read on main thread to
    /// force a frame when only scene-graph HTML content has changed.
    bool hasPendingHtmlWork() const;

private:
    // The renderer walks nodes/camera state directly through its graph
    // back-reference; it is the only class with private access.
    friend class SceneRenderer;

    void collectDestroyList(SceneNode* node, std::vector<uint32_t>& ids);

    /// Derive view/projection + cached intrinsics from the active camera
    /// node's world matrix. No-op when no camera node is active; a stale id
    /// (node destroyed) deactivates and keeps the last derived view. Called
    /// from tickAnimations (so the audio listener + JS see the fresh view),
    /// setActiveCamera, and the top of render().
    void applyActiveCamera();

    std::unique_ptr<SceneNode> root_;
    std::unordered_map<uint32_t, std::unique_ptr<SceneNode>> nodes_;

    // AI integration. aiWorldPin_ keeps the attached world's JS wrapper alive
    // for the duration of the attachment (see attachAIWorld).
    std::unique_ptr<AIWorldTicker> aiTicker_;
    std::shared_ptr<void> aiWorldPin_;
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

    // Active camera node by id (0 = none; ids are never reused, so a stale
    // id can only read as "destroyed"). See setActiveCamera/applyActiveCamera.
    uint32_t activeCameraId_ = 0;

    // Legacy 2D camera state (drives the CanvasScene 2D path)
    float cameraX_ = 0, cameraY_ = 0;
    float cameraZoom_ = 1.0f;

    // Canvas size for FBO
    int canvasWidth_ = 0, canvasHeight_ = 0;

    FBOTextureCallback fboTexCb_;
    GizmoProvider gizmoProvider_;

    // Scene-as-texture liveness token (lazily created; see
    // outputTextureSource()). Consumers hold weak_ptrs only.
    std::shared_ptr<OutputTextureSource> outputTexSource_;

    // JS-wrapper liveness token (created in the constructor; see
    // livenessToken()). Wrappers hold weak_ptrs only.
    std::shared_ptr<LivenessToken> liveToken_;

    // GL rendering: pipelines, FBOs, shadows, IBL, post stack. The renderer
    // never touches graph state in its destructor, so member order is not
    // load-bearing; it lives last simply to keep the hot node/camera state
    // at stable offsets.
    SceneRenderer renderer_{*this};
};

} // namespace bro::scene
