#pragma once

#include <glad/gl.h>

#include <bromath/frustum.h>

#include "scene/light_node.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {

class SceneGraph;
class SceneNode;
class MeshNode;
class InstancedMeshNode;
class Particles3DNode;
struct CustomShaderState;

/// Per-frame frustum-culling counters, reset at the top of every render3D().
/// "Drawn" counts nodes submitted to a pass (including nodes without valid
/// bounds, which always draw); "culled" counts nodes skipped by a frustum
/// test. Shadow counts are per caster x atlas tile (a caster drawn into two
/// cascades and skipped in one contributes drawn+=2, culled+=1).
struct CullStats {
    int meshDrawn = 0,       meshCulled = 0;
    int instancedDrawn = 0,  instancedCulled = 0;
    int splatDrawn = 0,      splatCulled = 0;
    int particlesDrawn = 0,  particlesCulled = 0;
    int billboardsDrawn = 0, billboardsCulled = 0;
    int shadowDrawn = 0,     shadowCulled = 0;
};

/// GL renderer for a SceneGraph's 3D content. Owns every GPU resource the
/// scene pipeline uses — mesh + instanced-mesh programs, the HDR mesh FBO,
/// shadow atlas (incl. CSM), IBL environment (cubemap, irradiance, prefilter,
/// BRDF LUT, skybox), billboard pipeline, and the post stack (bloom pre-pass,
/// tonemap, tilt-shift DOF) — plus the render *settings* that configure them
/// (fog, tonemap/exposure, ambient, wind, shadow quality, environment).
///
/// The graph owns one SceneRenderer by value and forwards its public render
/// API here; the renderer walks nodes through its graph back-reference.
/// All methods must run on the GL thread. Lazy init throughout: pipelines
/// and FBOs are created on first need, so a graph with no 3D content never
/// touches GL.
class SceneRenderer {
public:
    explicit SceneRenderer(SceneGraph& graph);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Render all 3D content (mesh / instanced / splat / billboard passes,
    /// shadows, skybox, post stack) into the mesh FBO + LDR output texture.
    /// No-op (beyond clearing hasMeshContent) when the graph has no visible
    /// 3D nodes. Called from SceneGraph::render() between the 2D framing.
    void render3D();

    /// Color texture of the 3D FBO (for compositing). 0 if no 3D content.
    GLuint meshFBOTexture() const { return meshColorTex_; }

    /// True if any 3D content was rendered this frame.
    bool hasMeshContent() const { return hasMeshContent_; }

    // Texture the compositor / readback should consume this frame: the
    // tilt-shift output when the pass ran, else the raw tonemap output.
    GLuint finalColorTex() const {
        return (tiltActive_ && postColorTex_) ? postColorTex_ : tonemapColorTex_;
    }

    /// Read RGBA8 pixels from the post-tonemap LDR FBO (top-down row order).
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

    // --- Render settings (see SceneGraph for API docs) ---

    void setFog(float start, float end, float r, float g, float b) {
        fogStart_ = start; fogEnd_ = end;
        fogColor_[0] = r; fogColor_[1] = g; fogColor_[2] = b;
    }

    /// Exponential-squared + height fog. `density` > 0 selects this mode
    /// over the linear start/end ramp (factor = 1 - exp(-(density*d)^2),
    /// d = camera distance past `startDistance`). `heightFalloff` > 0
    /// scales density by exp(-heightFalloff * worldY) per fragment, so low
    /// geometry sits deeper in fog. Color is shared with setFog. Pass
    /// density 0 to fall back to the linear mode (or fully off).
    void setFogExp(float density, float heightFalloff, float startDistance) {
        fogDensity_       = density < 0.0f ? 0.0f : density;
        fogHeightFalloff_ = heightFalloff < 0.0f ? 0.0f : heightFalloff;
        fogStartDist_     = startDistance < 0.0f ? 0.0f : startDistance;
    }

    enum class ToneMap : uint8_t { Linear, Reinhard, ACES };

    void setToneMap(ToneMap mode, float exposure, float gamma) {
        toneMap_ = mode; exposure_ = exposure; gamma_ = gamma;
    }
    ToneMap toneMap() const { return toneMap_; }
    float exposure() const { return exposure_; }

    void setAmbient(float r, float g, float b) {
        ambientColor_[0] = r; ambientColor_[1] = g; ambientColor_[2] = b;
    }

    void setTiltShift(bool enabled, float focusCenter, float focusWidth,
                      float feather, float strength, float saturation,
                      float contrast) {
        tiltEnabled_     = enabled;
        tiltFocusCenter_ = focusCenter;
        tiltFocusWidth_  = focusWidth;
        tiltFeather_     = feather;
        tiltStrength_    = strength;
        tiltSaturation_  = saturation;
        tiltContrast_    = contrast;
    }
    bool tiltShiftEnabled() const { return tiltEnabled_; }

    void setBloom(bool enabled, float threshold, float intensity, float strength) {
        bloomEnabled_   = enabled;
        bloomThreshold_ = threshold;
        bloomIntensity_ = intensity;
        bloomStrength_  = strength;
    }
    bool bloomEnabled() const { return bloomEnabled_; }

    /// Screen-space ambient occlusion. Computed at half-res from the
    /// resolved scene depth (hemisphere kernel + rotation noise, separable
    /// blur) and multiplied into the lit HDR image in the tonemap pass.
    /// `radius` is the world-space hemisphere radius, `intensity` scales
    /// how dark occlusion gets (0..1+, 1 = full AO), `bias` is the depth
    /// acceptance offset that suppresses self-occlusion acne.
    void setSSAO(bool enabled, float radius, float intensity, float bias) {
        ssaoEnabled_   = enabled;
        ssaoRadius_    = radius > 0.0f ? radius : 0.5f;
        ssaoIntensity_ = intensity < 0.0f ? 0.0f : intensity;
        ssaoBias_      = bias;
    }
    bool ssaoEnabled() const { return ssaoEnabled_; }

    void setWind(float dirX, float dirY, float dirZ,
                 float strength, float frequency) {
        windDir_[0] = dirX; windDir_[1] = dirY; windDir_[2] = dirZ;
        windStrength_ = strength;
        windFreq_ = frequency;
    }
    void advanceWindTime(float dt) { windTime_ += dt; }
    void resetWindTime() { windTime_ = 0.0f; }
    float windTime() const { return windTime_; }

    void setShadowQuality(int atlasSize, int pcfTaps) {
        shadowAtlasSize_ = atlasSize > 0 ? atlasSize : 8192;
        shadowPCFTaps_ = (pcfTaps == 1) ? 1 : 3;
        shadowAtlasDirty_ = true;
    }
    int shadowAtlasSize() const { return shadowAtlasSize_; }
    int shadowPCFTaps() const { return shadowPCFTaps_; }

    void setShowLightIcons(bool on) { showLightIcons_ = on; }
    bool showLightIcons() const { return showLightIcons_; }

    /// Frustum culling for the forward + shadow passes. Default on; the
    /// escape hatch exists for debugging and regression bisecting.
    void setFrustumCulling(bool on) { frustumCullingEnabled_ = on; }
    bool frustumCullingEnabled() const { return frustumCullingEnabled_; }

    /// Internal render-resolution multiplier (clamped 0.25-2.0). Every FBO
    /// chain (mesh HDR, tonemap, bloom, tilt-shift) resizes to canvas*scale
    /// on the next frame; the compositor samples the result at the CSS
    /// element box, so layout, picking and camera aspect are unaffected.
    void setRenderScale(float s) {
        renderScale_ = s < 0.25f ? 0.25f : (s > 2.0f ? 2.0f : s);
    }
    float renderScale() const { return renderScale_; }

    /// MSAA sample count for the HDR 3D passes. 0/1 = off; clamped to the
    /// driver's GL_MAX_SAMPLES at allocation time (the stored value is the
    /// request, not the clamp).
    void setMSAA(int samples) { msaaSamples_ = samples < 2 ? 0 : samples; }
    int msaaSamples() const { return msaaSamples_; }

    /// Culling counters from the most recent render3D().
    const CullStats& cullStats() const { return cullStats_; }

    // --- IBL environment ---

    bool loadEnvironment(const std::string& hdrPath);
    void clearEnvironment();
    bool hasEnvironment() const { return envCubemap_ != 0; }
    const std::string& environmentPath() const { return envPath_; }

    void  setEnvironmentIntensity(float i) { envIntensity_ = (i < 0.0f) ? 0.0f : i; }
    float environmentIntensity() const { return envIntensity_; }

    void  setEnvironmentRotation(float r) { envRotation_ = r; }
    float environmentRotation() const { return envRotation_; }

    // --- Custom mesh shaders ---

    /// Which mesh pipeline a custom-shader program variant targets. Static
    /// and Skinned share mesh.vert/mesh.frag (SKINNED define); Instanced
    /// splices into mesh_instanced.vert + the derived instanced fragment
    /// source. Each target caches its own program per chunk pair.
    enum class CustomShaderTarget : uint8_t { Static, Skinned, Instanced };

    /// Eagerly compile + cache the mesh program variant for a pair of user
    /// GLSL chunks (either may be empty). Returns true when the program
    /// linked (or was already cached); on failure returns false with the
    /// full driver log in errOut and caches nothing. GL thread only — the
    /// JS thread owns the main context, so setShader can validate at set
    /// time. `key` must be CustomShaderState::key for the same chunk pair
    /// (vertex + '\x1f' + fragment).
    ///
    /// A non-empty vertex chunk also eagerly compiles the matching shadow
    /// program variant (Static/Skinned targets) so displaced meshes cast
    /// displaced silhouettes. Shadow-variant failure is NOT an error — the
    /// caster falls back to the shared default shadow program with a
    /// warning (a chunk can legally reference mesh-pass-only symbols).
    bool compileCustomShader(CustomShaderTarget target,
                             const std::string& key,
                             const std::string& vertexChunk,
                             const std::string& fragmentChunk,
                             std::string& errOut);

private:
    // Per-program uniform locations for the PBR mesh pipeline (everything
    // renderMeshNode + the per-frame globals touch). One instance for the
    // regular mesh program and one for the skinned variant (same GLSL source,
    // SKINNED define), so both passes share renderMeshNode.
    struct MeshDrawLocs {
        GLint mvp = -1, model = -1, color = -1, emissive = -1,
              emissiveColor = -1, metallic = -1, roughness = -1, unlit = -1,
              twoSided = -1, subsurface = -1, alphaCutoff = -1,
              useVertexColor = -1, nearClip = -1, windMask = -1,
              useTexture = -1, baseColorTex = -1, normalMap = -1, mrMap = -1,
              aoMap = -1, emissiveMap = -1, hasTangent = -1,
              hasNormalMap = -1, hasMRMap = -1, hasAOMap = -1,
              hasEmissiveMap = -1, receivesShadow = -1,
              fogStart = -1, fogEnd = -1, fogColor = -1, ambient = -1,
              fogDensity = -1, fogHeightFalloff = -1, fogStartDist = -1,
              fogCamY = -1,
              windDir = -1, windStrength = -1, windTime = -1, windFreq = -1;
    };
    // Per-program uniform locations for the instanced mesh pipeline
    // (everything renderInstancedMeshNode + the per-frame globals touch).
    // The instanced vertex shader reads the model matrix from per-instance
    // attributes, so vp/cameraEye/instModel replace the per-mesh mvp/model.
    struct InstancedDrawLocs {
        GLint vp = -1, cameraEye = -1, instModel = -1, color = -1,
              emissive = -1, emissiveColor = -1, metallic = -1,
              roughness = -1, unlit = -1, useVertexColor = -1,
              nearClip = -1, useTexture = -1, baseColorTex = -1,
              normalMap = -1, mrMap = -1, aoMap = -1, emissiveMap = -1,
              hasTangent = -1, hasNormalMap = -1, hasMRMap = -1,
              hasAOMap = -1, hasEmissiveMap = -1, receivesShadow = -1,
              fogStart = -1, fogEnd = -1, fogColor = -1, ambient = -1,
              fogDensity = -1, fogHeightFalloff = -1, fogStartDist = -1,
              fogCamY = -1,
              atlasGrid = -1, alphaCutoff = -1;
    };
    struct MeshProgramLocs;  // lighting/shadow/IBL locs — defined below

    void renderMeshNode(MeshNode* mesh, const MeshDrawLocs& L);
    void renderInstancedMeshNode(InstancedMeshNode* mesh,
                                 const InstancedDrawLocs& L);
    void ensureInstancedMeshPipeline();
    void renderGaussianSplatNodes();
    void renderBillboardNode(SceneNode* node);

    // Upload the per-frame globals (fog, ambient, wind) to whichever mesh
    // program is currently bound. Defined in scene_renderer.cpp.
    void uploadMeshGlobals(const MeshDrawLocs& L);

    // Conservative world-space AABB for a cullable node (Mesh incl. skinned,
    // InstancedMesh, GaussianSplat, Particles3D). Returns false when the node
    // has no valid bounds — such nodes draw unconditionally. Defined in
    // scene_renderer.cpp.
    bool nodeWorldBounds(SceneNode* n, bromath::AABB3& out) const;

    // Camera-frustum test for the forward walks: true when the node is
    // provably outside the view and safe to skip. Never true while culling
    // is disabled or when the node has no valid bounds.
    bool cameraCulled(SceneNode* n) const;

    // Query the full uniform surface of a mesh program (regular or skinned —
    // both link mesh.frag, so the surface is identical) into a draw-locs +
    // light-locs pair. Defined in scene_renderer_mesh.cpp.
    void queryMeshUniformLocs(GLuint prog, MeshDrawLocs& d, MeshProgramLocs& l);

    // Same, for a program linking mesh_instanced.vert + the derived
    // instanced fragment source. Defined in scene_renderer_instanced.cpp.
    void queryInstancedUniformLocs(GLuint prog, InstancedDrawLocs& d,
                                   MeshProgramLocs& l);

    // --- Mesh GL pipeline (lazy init) ---
    void ensureMeshPipeline();
    void ensureSkinnedMeshPipeline();
    void ensureMeshFBO();
    void destroyMeshFBO();

    // Scaled render-target size: CSS canvas size * renderScale_, min 1.
    // Everything that sizes an internal FBO goes through these; the canvas
    // (CSS) size stays the contract for picking, aspect and compositing.
    int targetWidth() const;
    int targetHeight() const;

    // --- MSAA HDR target (lazy, only while msaaSamples_ >= 2) ---
    // Sets msaaActive_ for the frame. Defined in scene_renderer.cpp.
    void ensureMSAAFBO();
    void destroyMSAAFBO();

    // --- Soft-particle scene-depth snapshot (lazy) ---
    // Defined in scene_renderer_particles.cpp.
    void ensureSceneDepthCopy();
    void destroySceneDepthCopy();

    // --- Billboard GL pipeline (lazy init) ---
    void ensureBillboardPipeline();

    // --- 3D particle pipeline (lazy init) ---
    // One shared program + unit-quad VBO; each Particles3DNode owns its VAO
    // and instance buffer. Defined in scene_renderer_particles.cpp.
    void ensureParticlePipeline();
    void renderParticles3DNodes();

    // --- Tonemap pipeline (lazy init) ---
    void ensureTonemapPipeline();
    void ensureTonemapFBO();
    void destroyTonemapFBO();
    void runTonemapPass();

    // --- Tilt-shift DOF post pass (lazy init) ---
    void ensureTiltShiftPipeline();
    void ensureTiltShiftFBOs();
    void destroyTiltShiftFBOs();
    void runTiltShiftPass();

    // --- Bloom pre-pass (HDR, runs before tonemap) ---
    void ensureBloomPipeline();
    void ensureBloomFBOs();
    void destroyBloomFBOs();
    // Bright-pass + blur into bloomTex_[0]; returns true if a glow is ready.
    bool runBloomPrePass();

    // --- SSAO pre-pass (half-res AO from resolved depth, before tonemap) ---
    void ensureSSAOPipeline();
    void ensureSSAOFBOs();
    void destroySSAOFBOs();
    // AO estimate + blur into ssaoTex_[0]; returns true when AO is ready.
    bool runSSAOPass();

    // --- Light collection (rebuilt per frame) ---
    void collectLights(std::vector<LightNode*>& out) const;

    // Implicit directional sun used when the scene declares no lights, so
    // meshes are never pitch black. Per-renderer (configured once in the
    // constructor, never mutated afterwards) — a shared process-global would
    // be mutable state crossing renderer instances.
    LightNode implicitSun_;

    // Bundle of uniform locations the lighting/shadow/IBL upload pokes at.
    // One instance is filled in for the regular mesh program and another for
    // the instanced mesh program, so uploadLights can target either.
    struct MeshProgramLocs {
        GLint lightCount = -1;
        GLint lightType = -1;
        GLint lightPos = -1;
        GLint lightDir = -1;
        GLint lightColor = -1;
        GLint lightIntensity = -1;
        GLint lightRange = -1;
        GLint lightSpotCos = -1;
        GLint lightShadowSlot = -1;
        GLint lightShadowSlotCount = -1;
        GLint lightCascadeSplit = -1;
        GLint shadowAtlas = -1;
        GLint shadowMatrix = -1;
        GLint shadowAtlasRect = -1;
        GLint shadowBias = -1;
        GLint shadowAtlasTexel = -1;
        GLint shadowPCFTaps = -1;
        GLint iblEnabled = -1;
        GLint iblIrradiance = -1;
        GLint iblPrefilter = -1;
        GLint iblBRDF = -1;
        GLint iblIntensity = -1;
        GLint iblRotation = -1;
        GLint iblPrefilterMaxLOD = -1;
    };
    MeshProgramLocs meshLocs_;
    MeshProgramLocs meshInstLocs_;
    MeshProgramLocs meshSkinnedLocs_;
    void uploadLights(const std::vector<LightNode*>& lights,
                      const MeshProgramLocs& locs);

    // --- Custom-shader program cache -----------------------------------
    // One linked mesh-program variant per distinct (target, chunk pair),
    // keyed by a target tag + the chunk sources (vertex + '\x1f' +
    // fragment), so meshes with identical shaders share a program per
    // target. Entries live until renderer teardown — no eviction; a scene
    // cycling through many distinct shader sources holds them all (cheap: a
    // GL program + a few locs structs each). userLocs lazily caches
    // glGetUniformLocation results for user uniforms (misses cache as -1 so
    // a typo'd name is one query, not one per draw). `draw` is valid for
    // Static/Skinned entries, `instDraw` for Instanced ones.
    struct CustomProgramEntry {
        GLuint prog = 0;
        MeshDrawLocs draw;
        InstancedDrawLocs instDraw;
        MeshProgramLocs locs;
        std::unordered_map<std::string, GLint> userLocs;
    };
    std::unordered_map<std::string, CustomProgramEntry> customPrograms_;

    // Look up (compiling on miss) the program for a (target, chunk pair).
    // Returns nullptr with the driver log in errOut on compile/link failure
    // (nothing cached — the next call retries). Pointer stays valid until
    // teardown (unordered_map nodes are stable across rehash). Defined in
    // scene_renderer_mesh.cpp (owns the embedded mesh shader sources).
    CustomProgramEntry* ensureCustomProgram(CustomShaderTarget target,
                                            const std::string& key,
                                            const std::string& vertexChunk,
                                            const std::string& fragmentChunk,
                                            std::string* errOut);

    // Upload one node's user-uniform values to `prog` (must be bound),
    // caching locations in `cache`. Defined in scene_renderer_mesh.cpp.
    void uploadUserUniforms(GLuint prog,
                            std::unordered_map<std::string, GLint>& cache,
                            const CustomShaderState* st);

    // --- Custom shadow-program cache ------------------------------------
    // Depth-only shadow.vert variants with a user vertex chunk spliced in
    // (per Static/Skinned flavour), so vertex-displaced meshes cast the
    // displaced silhouette. Keyed by flavour tag + vertex chunk ONLY —
    // fragment-only shaders never allocate one (they keep the shared
    // default shadow program). Unlike the mesh cache, failures are cached
    // (prog == 0): a chunk can legally reference mesh-pass-only symbols, in
    // which case the caster falls back to the default shadow program with a
    // one-time warning instead of a per-frame recompile attempt.
    struct CustomShadowEntry {
        GLuint prog = 0;
        GLint mvp = -1, model = -1, windDir = -1, windStrength = -1,
              windTime = -1, windFreq = -1, windMask = -1;
        std::unordered_map<std::string, GLint> userLocs;
    };
    std::unordered_map<std::string, CustomShadowEntry> customShadowPrograms_;

    // Look up (compiling on miss) the shadow variant for a vertex chunk.
    // Returns nullptr when the variant failed to compile (cached failure —
    // use the default shadow program). Defined in scene_renderer_shadow.cpp
    // (owns the embedded shadow shader sources).
    CustomShadowEntry* ensureCustomShadowProgram(bool skinned,
                                                 const std::string& vertexChunk);

    // --- Shadow pipeline (lazy init) ---
    // Atlas-tiled shadow maps: a single big depth texture sub-divided into N
    // square tiles. Each shadow-casting light gets one or more tiles (1 for
    // directional/spot, 6 for point cube faces, N for CSM cascades). All
    // mesh fragments sample from one sampler2DShadow keyed by per-light slot.
    void ensureShadowPipeline();
    void ensureShadowInstancedPipeline();
    void ensureShadowSkinnedPipeline();
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

    // --- IBL environment internals ---
    void ensureEnvConvertPipeline();
    bool runEquirectToCubemap(GLuint equirectTex, GLuint cubemap, int faceSize);
    void ensureSkyboxPipeline();
    void renderSkyboxPass();
    void ensureIrradiancePipeline();
    bool runIrradianceConvolution();
    void ensurePrefilterPipeline();
    bool runPrefilterConvolution();
    void ensureBRDFLUT();           // 2D RG16F LUT, baked once on first need

    void ensureFallbackTextures();

    /// The graph this renderer draws. Outlives the renderer (the graph owns
    /// it by value); nodes/camera/canvas state are read through it.
    SceneGraph& graph_;

    // Mesh rendering GL resources (shared across all MeshNodes). meshDraw_
    // holds the uniform locations for the regular program, meshSkinnedDraw_
    // for the SKINNED-variant program used by SkinnedMeshNode.
    GLuint meshProgram_ = 0;
    MeshDrawLocs meshDraw_;
    GLuint meshSkinnedProgram_ = 0;
    MeshDrawLocs meshSkinnedDraw_;

    // Instanced mesh program (vertex shader reads model matrix from per-instance
    // attributes; fragment shader is derived from the regular mesh fragment
    // source). meshInstDraw_ holds its uniform locations, the instanced
    // analog of meshDraw_; light locs live in meshInstLocs_ below.
    GLuint meshInstancedProgram_ = 0;
    InstancedDrawLocs meshInstDraw_;

    // Mesh FBO. The depth-stencil attachment is a texture (not an RBO) so
    // the soft-particle pass can sample scene depth; the tonemap FBO
    // re-attaches it for the post-tonemap unlit overlay's depth test.
    GLuint meshFBO_ = 0;
    GLuint meshColorTex_ = 0;
    GLuint meshDepthTex_ = 0;   // DEPTH24_STENCIL8
    int meshFBOWidth_ = 0, meshFBOHeight_ = 0;

    // MSAA HDR target: multisampled renderbuffers the HDR passes render
    // into while MSAA is on, resolved into meshColorTex_ / meshDepthTex_
    // via glBlitFramebuffer. See render3D() for the resolve ordering.
    GLuint msaaFBO_ = 0;
    GLuint msaaColorRBO_ = 0;
    GLuint msaaDepthRBO_ = 0;
    int msaaWidth_ = 0, msaaHeight_ = 0;
    int msaaSamplesAllocated_ = 0;
    bool msaaActive_ = false;   // per-frame: MSAA FBO complete this frame

    // Soft-particle depth snapshot: the particle pass samples this copy of
    // the opaque scene depth — sampling a texture attached to the current
    // draw FBO is a feedback loop in strict GL 3.3 even with depth writes
    // off, so it can never sample meshDepthTex_ directly.
    GLuint sceneDepthCopyFBO_ = 0;
    GLuint sceneDepthCopyTex_ = 0;
    int sceneDepthCopyWidth_ = 0, sceneDepthCopyHeight_ = 0;

    // Render-target settings
    float renderScale_ = 1.0f;   // internal-resolution multiplier
    int msaaSamples_ = 0;        // requested sample count; 0/1 = off

    bool hasMeshContent_ = false;

    // Per-frame: true while the post-tonemap unlit overlay pass is drawing
    // (into tonemapFBO_). renderMeshNode uses it to refuse binding an
    // external baseColor texture that IS tonemapColorTex_ — i.e. a mesh
    // sampling its own scene's output — since sampling the bound draw
    // attachment is a GL feedback loop (undefined behavior). The lit mesh
    // pass needs no guard: it draws into meshFBO_, never into the LDR
    // output an external provider can hand out.
    bool unlitOverlayActive_ = false;

    // Distance fog. Linear ramp (start/end) or, when fogDensity_ > 0,
    // exponential-squared height fog (see setFogExp).
    float fogStart_ = 0.0f;
    float fogEnd_ = 0.0f;
    float fogColor_[3] = {0.0f, 0.0f, 0.0f};
    float fogDensity_ = 0.0f;
    float fogHeightFalloff_ = 0.0f;
    float fogStartDist_ = 0.0f;

    // Tonemap + exposure
    ToneMap toneMap_ = ToneMap::ACES;
    float exposure_ = 1.0f;
    float gamma_ = 2.2f;
    float ambientColor_[3] = {0.03f, 0.03f, 0.03f};

    // Wind sway (vertex shader displacement)
    float windDir_[3] = {1.0f, 0.0f, 0.0f};
    float windStrength_ = 0.0f;
    float windFreq_ = 1.5f;
    float windTime_ = 0.0f;

    // Editor affordance: render a marker icon per LightNode and include
    // them in raycast results.
    bool showLightIcons_ = false;

    // --- Frustum culling ---
    bool frustumCullingEnabled_ = true;
    // Per-frame state set at the top of render3D(): world-space camera
    // frustum (valid while cullingActive_) + drawn/culled counters.
    bool cullingActive_ = false;
    bromath::Frustum cameraFrustum_;
    CullStats cullStats_;

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
    GLint tmUBloomTex_ = -1;
    GLint tmUBloomIntensity_ = -1;
    GLint tmUSSAOTex_ = -1;
    GLint tmUSSAOIntensity_ = -1;

    // --- SSAO pre-pass ---
    bool  ssaoEnabled_   = false;
    float ssaoRadius_    = 0.5f;
    float ssaoIntensity_ = 1.0f;
    float ssaoBias_      = 0.025f;

    // Half-res R8 AO + blur ping-pong; 4x4 rotation noise (RG, repeat).
    GLuint ssaoFBO_[2] = {0, 0};
    GLuint ssaoTex_[2] = {0, 0};
    int    ssaoWidth_  = 0, ssaoHeight_ = 0;
    GLuint ssaoNoiseTex_ = 0;
    float  ssaoKernel_[16 * 3] = {};   // hemisphere samples, built once

    GLuint ssaoProgram_ = 0;
    GLint  aoUDepth_      = -1;
    GLint  aoUNoise_      = -1;
    GLint  aoUProj_       = -1;
    GLint  aoUInvProj_    = -1;
    GLint  aoUKernel_     = -1;
    GLint  aoURadius_     = -1;
    GLint  aoUBias_       = -1;
    GLint  aoUNoiseScale_ = -1;

    // --- Tilt-shift DOF post pass ---
    // Params (see setTiltShift). Disabled by default so the pass is a no-op
    // and the compositor reads tonemapColorTex_ unchanged.
    bool  tiltEnabled_     = false;
    float tiltFocusCenter_ = 0.5f;
    float tiltFocusWidth_  = 0.12f;
    float tiltFeather_     = 0.25f;
    float tiltStrength_    = 2.0f;
    float tiltSaturation_  = 1.0f;
    float tiltContrast_    = 1.0f;
    // Set true by runTiltShiftPass when it produced postColorTex_ this frame;
    // finalColorTex() keys off it. Cleared when the pass is skipped.
    bool  tiltActive_      = false;

    // Separable-blur ping-pong (half-res) + full-res composite target.
    GLuint blurFBO_[2]   = {0, 0};
    GLuint blurTex_[2]   = {0, 0};
    int    blurWidth_    = 0, blurHeight_ = 0;
    GLuint postFBO_      = 0;
    GLuint postColorTex_ = 0;
    int    postWidth_    = 0, postHeight_ = 0;

    GLuint blurProgram_  = 0;
    GLint  blUTex_       = -1;
    GLint  blUDir_       = -1;   // texel step * radius (vec2)
    GLuint tiltProgram_  = 0;
    GLint  tsUSharp_     = -1;
    GLint  tsUBlur_      = -1;
    GLint  tsUFocusCenter_ = -1;
    GLint  tsUFocusWidth_  = -1;
    GLint  tsUFeather_     = -1;
    GLint  tsUSaturation_  = -1;
    GLint  tsUContrast_    = -1;

    // --- HDR bloom pre-pass ---
    bool  bloomEnabled_   = false;
    float bloomThreshold_ = 1.0f;
    float bloomIntensity_ = 0.0f;
    float bloomStrength_  = 2.0f;
    bool  bloomActive_    = false;   // bloom ready in bloomTex_[0] this frame

    // Half-res HDR (RGBA16F) bright-pass + ping-pong blur.
    GLuint bloomFBO_[2] = {0, 0};
    GLuint bloomTex_[2] = {0, 0};
    int    bloomWidth_  = 0, bloomHeight_ = 0;

    GLuint bloomBrightProgram_ = 0;
    GLint  bbpUTex_       = -1;
    GLint  bbpUThreshold_ = -1;

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
    GLuint shadowInstancedProgram_ = 0;
    GLint  shadowInstULightVP_ = -1;
    GLint  shadowInstUModel_ = -1;
    GLuint shadowSkinnedProgram_ = 0;
    GLint  shadowSkinnedUMVP_ = -1;
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
    // Skinned casters render with the SKINNED shadow program so their
    // shadows deform with the palette instead of staying in bind pose.
    // Casters whose custom shader has a VERTEX chunk split into the custom
    // lists (sorted by chunk source so programs bind once per group) and
    // render with the spliced shadow variant — displaced silhouettes.
    // Fragment-only custom shaders stay in the default lists.
    std::vector<MeshNode*> shadowCasters_;
    std::vector<MeshNode*> shadowSkinnedCasters_;
    std::vector<MeshNode*> shadowCustomCasters_;
    std::vector<MeshNode*> shadowSkinnedCustomCasters_;
    std::vector<InstancedMeshNode*> shadowInstancedCasters_;

    // 1×1 fallback textures bound to sampler units when the real textures
    // aren't available. Prevents GL_INVALID_OPERATION on strict core-profile
    // drivers (macOS GL 4.1) when IBL/shadows aren't active: unbound samplers
    // alias unit 0 and cross sampler types (sampler2D / samplerCube /
    // sampler2DShadow) which the spec forbids at draw time.
    GLuint fallback2D_ = 0;       // white RGBA8 2D
    GLuint fallbackCube_ = 0;     // white RGBA8 cube
    GLuint fallbackShadow_ = 0;   // depth24 2D with COMPARE_REF_TO_TEXTURE

    // --- IBL environment state ---
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

    // --- 3D particle pipeline (lazy init) ---
    GLuint particleProgram_ = 0;
    GLuint particleQuadVBO_ = 0;
    GLint pUVP_ = -1;
    GLint pUModel_ = -1;
    GLint pUCameraEye_ = -1;
    GLint pURight_ = -1;
    GLint pUUp_ = -1;
    GLint pUFlipGrid_ = -1;
    GLint pUMode_ = -1;
    GLint pUTex_ = -1;
    // Soft-particle uniforms (scene-depth fade)
    GLint pUSceneDepth_ = -1;
    GLint pUViewport_ = -1;
    GLint pUDepthRange_ = -1;
    GLint pUPerspective_ = -1;
    GLint pUSoftDistance_ = -1;

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
