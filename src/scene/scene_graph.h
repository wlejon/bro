#pragma once

#include "scene/scene_node.h"
#include "scene/shape_node.h"
#include "scene/sprite_node.h"
#include "scene/physics_node.h"
#include "scene/mesh_node.h"
#include "scene/html_node.h"
#include "scene/light_node.h"
#include "scene/particle_node.h"
#include "scene/tilemap_node.h"
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
    LightNode* createLight(const std::string& name = "");
    ParticleNode* createParticles(const std::string& name = "");
    TilemapNode* createTilemap(const std::string& name = "");

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
    GLuint meshFBOTexture() const { return meshColorTex_; }

    /// Returns true if any MeshNodes were rendered this frame.
    bool hasMeshContent() const { return hasMeshContent_; }

    /// Read RGBA8 pixels from the post-tonemap LDR FBO. Used by offscreen
    /// capture (artstation defineScene): renders 3D content with alpha=0 in
    /// uncovered regions, so the readback is suitable for compositing into a
    /// 2D canvas cell via putImageData. Pixels are returned in top-down row
    /// order (matches CSS / ImageData), unlike GL's bottom-up native layout.
    /// Returns empty vector if the tonemap FBO hasn't been populated yet
    /// (e.g. no render() with 3D content has run). Must be called on the GL
    /// thread.
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

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

    /// When true, setCanvasSize() rebuilds the projection matrix using the
    /// new canvas aspect ratio (and the stored fovY/near/far). Use this when
    /// the caller wants the camera aspect to track the viewport; pin an
    /// explicit aspect in setCamera() and leave this false for fixed-aspect
    /// / cinematic cameras.
    void setCameraAspectFollowsCanvas(bool on) { cameraAspectFollowsCanvas_ = on; }

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

    /// Set distance fog parameters. start/end in world units, color is RGB [0,1].
    void setFog(float start, float end, float r, float g, float b) {
        fogStart_ = start; fogEnd_ = end;
        fogColor_[0] = r; fogColor_[1] = g; fogColor_[2] = b;
    }

    /// Tone mapping mode applied when composing the HDR mesh FBO to the
    /// caller-facing LDR texture. ACES matches modern filmic defaults;
    /// Reinhard is a cheaper fallback; Linear is raw clamp to 0-1.
    enum class ToneMap : uint8_t { Linear, Reinhard, ACES };

    /// Configure tonemap + exposure. Exposure is a pre-tonemap multiplier
    /// (1.0 = neutral, 2.0 = +1 stop). `gamma` is the post-tonemap output
    /// gamma; pass 1.0 to leave linear output (default 2.2 for sRGB).
    void setToneMap(ToneMap mode, float exposure = 1.0f, float gamma = 2.2f) {
        toneMap_ = mode; exposure_ = exposure; gamma_ = gamma;
    }
    ToneMap toneMap() const { return toneMap_; }
    float exposure() const { return exposure_; }

    /// Ambient fill added to every fragment after per-light contributions
    /// (a placeholder until IBL lands). RGB in linear 0-1.
    void setAmbient(float r, float g, float b) {
        ambientColor_[0] = r; ambientColor_[1] = g; ambientColor_[2] = b;
    }

    // --- Shadow mapping ---

    /// Configure shadow atlas and quality. `atlasSize` is the side length of
    /// the square depth texture (e.g. 4096). `pcfTaps` is 1 (single sample)
    /// or 3 (3x3 PCF, default). Defaults are sane — call only to tune.
    void setShadowQuality(int atlasSize, int pcfTaps) {
        shadowAtlasSize_ = atlasSize > 0 ? atlasSize : 8192;
        shadowPCFTaps_ = (pcfTaps == 1) ? 1 : 3;
        shadowAtlasDirty_ = true;
    }
    int shadowAtlasSize() const { return shadowAtlasSize_; }
    int shadowPCFTaps() const { return shadowPCFTaps_; }

    /// Editor affordance: when true, every LightNode renders a small
    /// kind-specific marker billboard at its world position (visible in
    /// the 3D FBO, depth-tested against geometry). Also makes lights
    /// pickable via `raycast()`: hits return the LightNode as `hit.node`.
    void setShowLightIcons(bool on) { showLightIcons_ = on; }
    bool showLightIcons() const { return showLightIcons_; }

    // --- IBL environment ---

    /// Load an HDR equirectangular image (.hdr) and convert it to a 512²
    /// cubemap that backs both skybox rendering and IBL precompute
    /// (irradiance + prefilter, added in later passes). Returns true on
    /// success; on failure the previous environment is kept. Pass an empty
    /// path to clear. Must be called on the GL thread (JS bindings already
    /// satisfy this).
    bool loadEnvironment(const std::string& hdrPath);
    void clearEnvironment();
    bool hasEnvironment() const { return envCubemap_ != 0; }
    const std::string& environmentPath() const { return envPath_; }

    /// Multiplier applied to all IBL contributions (skybox + irradiance +
    /// prefilter). 1.0 = neutral. Tune independently of sun intensity.
    void  setEnvironmentIntensity(float i) { envIntensity_ = (i < 0.0f) ? 0.0f : i; }
    float environmentIntensity() const { return envIntensity_; }

    /// Y-axis rotation of the environment in radians. Useful for aligning
    /// the visible sun in the HDR with the engine's directional sun light.
    void  setEnvironmentRotation(float r) { envRotation_ = r; }
    float environmentRotation() const { return envRotation_; }

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

    // --- Tonemap pipeline (lazy init) ---
    void ensureTonemapPipeline();
    void ensureTonemapFBO();
    void destroyTonemapFBO();
    void runTonemapPass();

    // --- Light collection (rebuilt per frame) ---
    void collectLights(std::vector<LightNode*>& out) const;
    void uploadLights(const std::vector<LightNode*>& lights);

    // --- Shadow pipeline (lazy init) ---
    // Atlas-tiled shadow maps: a single big depth texture sub-divided into N
    // square tiles. Each shadow-casting light gets one or more tiles (1 for
    // directional/spot, 6 for point cube faces, N for CSM cascades). All
    // mesh fragments sample from one sampler2DShadow keyed by per-light slot.
    void ensureShadowPipeline();
    void ensureShadowAtlas();
    void destroyShadowAtlas();

    // Decide which lights cast shadows this frame, allocate atlas tiles, and
    // compute world->shadow-clip matrices. Run after collectLights() and the
    // camera has been set. Populates the shadow* per-frame arrays.
    void prepareShadows(const std::vector<LightNode*>& lights);

    // Render every shadow-casting mesh into each allocated atlas tile using
    // the depth-only shadow program. Leaves shadowAtlasFBO_ unbound on exit.
    void renderShadowPass();

    // Compute the world-space AABB enclosing all shadow-casting meshes.
    // Used to fit directional shadow frustums; returns empty BBox if none.
    struct WorldAABB { float min[3]; float max[3]; bool empty; };
    WorldAABB computeShadowCasterBounds() const;

    // Render a ringed-disc billboard for one light. Used by the editor-
    // affordance pass gated on showLightIcons_.
    void renderLightIcon(LightNode* light);

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
    GLint uHasTangent_ = -1;
    GLint uHasNormalMap_ = -1;
    GLint uHasMRMap_ = -1;
    GLint uHasAOMap_ = -1;
    GLint uHasEmissiveMap_ = -1;
    GLint uNormalMap_ = -1;
    GLint uMRMap_ = -1;
    GLint uAOMap_ = -1;
    GLint uEmissiveMap_ = -1;
    GLint uReceivesShadow_ = -1;
    GLint uFogStart_ = -1;
    GLint uFogEnd_ = -1;
    GLint uFogColor_ = -1;
    GLint uNearClip_ = -1;
    GLint uMetallic_ = -1;
    GLint uRoughness_ = -1;
    GLint uEmissiveColor_ = -1;
    GLint uAmbient_ = -1;
    GLint uUnlit_ = -1;
    GLint uLightCount_ = -1;
    GLint uLightType_ = -1;
    GLint uLightPos_ = -1;
    GLint uLightDirArr_ = -1;
    GLint uLightColor_ = -1;
    GLint uLightIntensity_ = -1;
    GLint uLightRange_ = -1;
    GLint uLightSpotCos_ = -1;

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

    // Tonemap + exposure
    ToneMap toneMap_ = ToneMap::ACES;
    float exposure_ = 1.0f;
    float gamma_ = 2.2f;
    float ambientColor_[3] = {0.03f, 0.03f, 0.03f};

    // Editor affordance: render a marker icon per LightNode and include
    // them in raycast results.
    bool showLightIcons_ = false;

    // Tonemap FBO (LDR output, consumed by the compositor)
    GLuint tonemapFBO_ = 0;
    GLuint tonemapColorTex_ = 0;
    int tonemapFBOWidth_ = 0, tonemapFBOHeight_ = 0;

    // Tonemap program
    GLuint tonemapProgram_ = 0;
    GLuint tonemapVAO_ = 0;
    GLuint tonemapVBO_ = 0;
    GLint tmUTex_ = -1;
    GLint tmUExposure_ = -1;
    GLint tmUGamma_ = -1;
    GLint tmUMode_ = -1;

    // --- Shadow pipeline state ---
    // Hard cap: 16 atlas tiles. A typical scene budget is 1 directional
    // (1-4 cascades) + a few spots/points; overflow lights silently render
    // unshadowed.
    static constexpr int kMaxShadowTiles = 16;

    int shadowAtlasSize_ = 8192;
    int shadowPCFTaps_ = 3;       // 1 or 3 (3x3 PCF)
    bool shadowAtlasDirty_ = true;

    GLuint shadowProgram_ = 0;
    GLint  shadowUMVP_ = -1;
    GLuint shadowAtlasFBO_ = 0;
    GLuint shadowAtlasTex_ = 0;
    int    shadowAtlasAllocated_ = 0;  // current tex side; 0 if none

    // Per-frame shadow data, populated by prepareShadows(). Indexed by slot.
    int   shadowTileCount_ = 0;
    float shadowMatrixCamRel_[kMaxShadowTiles][16] = {};
    float shadowAtlasRect_[kMaxShadowTiles][4]     = {};   // origin.xy, size.xy in [0,1]
    float shadowBias_[kMaxShadowTiles][2]          = {};   // const, normal-bias world units

    // Per-light shadow slot (-1 if unshadowed). Indexed by light index.
    int lightShadowSlot_[32] = {};
    // For directional CSM: 1..4 cascades, each occupies a contiguous slot.
    int   lightShadowSlotCount_[32] = {};
    // Cascade FAR distances in view space; .x = cascade 0 far, etc. The
    // last cascade's far is implicitly +inf (any fragment further than
    // .z still samples the last cascade).
    float lightCascadeSplit_[32][4] = {};

    // For prepareShadows: matrices to render into the atlas (one per tile).
    // World-space (no camera-relative bake) — used by the shadow caster pass.
    float shadowRenderMatrix_[kMaxShadowTiles][16] = {};
    // Which light owns each tile, for routing the caster draws.
    LightNode* shadowTileLight_[kMaxShadowTiles] = {};

    // Cache per-frame shadow caster list; rebuilt at top of prepareShadows.
    std::vector<MeshNode*> shadowCasters_;

    // Mesh shader uniform locations for shadow data.
    GLint uShadowAtlas_ = -1;
    GLint uShadowMatrix_ = -1;
    GLint uShadowAtlasRect_ = -1;
    GLint uShadowBiasArr_ = -1;
    GLint uLightShadowSlot_ = -1;
    GLint uLightShadowSlotCount_ = -1;
    GLint uLightCascadeSplit_ = -1;
    GLint uShadowAtlasTexel_ = -1;
    GLint uShadowPCFTaps_ = -1;

    // 1×1 fallback textures bound to sampler units when the real textures
    // aren't available. Prevents GL_INVALID_OPERATION on strict core-profile
    // drivers (macOS GL 4.1) when IBL/shadows aren't active: unbound samplers
    // alias unit 0 and cross sampler types (sampler2D / samplerCube /
    // sampler2DShadow) which the spec forbids at draw time.
    GLuint fallback2D_ = 0;       // white RGBA8 2D
    GLuint fallbackCube_ = 0;     // white RGBA8 cube
    GLuint fallbackShadow_ = 0;   // depth24 2D with COMPARE_REF_TO_TEXTURE
    void ensureFallbackTextures();

    // IBL uniforms in the mesh program
    GLint uIBLEnabled_ = -1;
    GLint uIBLIrradiance_ = -1;
    GLint uIBLPrefilter_ = -1;
    GLint uIBLBRDF_ = -1;
    GLint uIBLIntensity_ = -1;
    GLint uIBLRotation_ = -1;
    GLint uIBLPrefilterMaxLOD_ = -1;

    // --- IBL environment ---
    void ensureEnvConvertPipeline();
    bool runEquirectToCubemap(GLuint equirectTex, GLuint cubemap, int faceSize);
    void ensureSkyboxPipeline();
    void renderSkyboxPass();
    void ensureIrradiancePipeline();
    bool runIrradianceConvolution();
    void ensurePrefilterPipeline();
    bool runPrefilterConvolution();
    void ensureBRDFLUT();           // 2D RG16F LUT, baked once on first need

    GLuint envCubemap_ = 0;          // 512² RGBA16F cube, 6 faces, mipmapped
    int    envCubemapSize_ = 0;
    GLuint envIrradianceCube_ = 0;   // 32² RGBA16F cube, cosine-convolved diffuse
    int    envIrradianceSize_ = 32;
    GLuint envPrefilterCube_ = 0;    // 256² RGBA16F cube, GGX-prefilter per mip
    int    envPrefilterSize_ = 256;
    int    envPrefilterMips_ = 6;    // mip 0..5 → roughness 0.0, 0.2, 0.4, 0.6, 0.8, 1.0
    GLuint brdfLUT_ = 0;             // 512² RG16F, env-independent (Karis split-sum)
    int    brdfLUTSize_ = 512;
    std::string envPath_;
    float  envIntensity_ = 1.0f;
    float  envRotation_ = 0.0f;

    // Equirect→cubemap converter (lazy init)
    GLuint envConvertProgram_ = 0;
    GLuint envConvertVAO_ = 0;
    GLuint envConvertVBO_ = 0;
    GLuint envConvertFBO_ = 0;
    GLint  envCvUFace_ = -1;
    GLint  envCvUEquirect_ = -1;

    // Irradiance convolver (lazy init, reuses envConvert FBO/VAO)
    GLuint irrConvProgram_ = 0;
    GLint  irrCvUEnv_ = -1;
    GLint  irrCvUFace_ = -1;

    // GGX prefilter (lazy init, reuses envConvert FBO/VAO)
    GLuint prefilterProgram_ = 0;
    GLint  pfUEnv_ = -1;
    GLint  pfUFace_ = -1;
    GLint  pfURoughness_ = -1;
    GLint  pfUEnvSize_ = -1;

    // BRDF LUT bake (lazy init, reuses envConvert FBO/VAO)
    GLuint brdfLUTProgram_ = 0;

    // Skybox draw pipeline (lazy init)
    GLuint skyboxProgram_ = 0;
    GLuint skyboxVAO_ = 0;
    GLuint skyboxVBO_ = 0;
    GLint  skyUViewToWorld_ = -1;
    GLint  skyUTanHalfFovY_ = -1;
    GLint  skyUAspect_ = -1;
    GLint  skyUEnv_ = -1;
    GLint  skyUIntensity_ = -1;
    GLint  skyURotation_ = -1;

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
    GLint bbUUvMin_ = -1;
    GLint bbUUvMax_ = -1;
};

} // namespace bro::scene
