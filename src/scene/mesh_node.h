#pragma once

#include "scene/custom_shader.h"
#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromesh/mesh_data.h>
#include <bromesh/analysis/bvh.h>
#include <glad/gl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

class SkinnedMeshNode;

/// A renderable 3D mesh node. Holds bromesh::MeshData and owns GL resources.
/// Renders into a shared FBO owned by SceneGraph (set up during render pass).
class MeshNode : public SceneNode {
public:
    explicit MeshNode(const std::string& name = "");
    ~MeshNode() override;

    MeshNode(const MeshNode&) = delete;
    MeshNode& operator=(const MeshNode&) = delete;

    Type type() const override { return Type::Mesh; }
    void onRender(SceneGraph& graph) override;

    /// Downcast hook: non-null when this node is a SkinnedMeshNode. Skinned
    /// meshes deliberately keep Type::Mesh so every Mesh-typed walk (shadow
    /// gather, bounds, raycast, material setters) treats them uniformly; the
    /// renderer routes them to the skinned program via this hook instead of
    /// a separate Type.
    virtual SkinnedMeshNode* asSkinnedMesh() { return nullptr; }

    // --- Draw mode ---

    /// What GL primitive the index buffer encodes. Triangles is the default
    /// and matches every existing call site. Lines reinterprets `indices` as
    /// pairs of endpoint indices and issues GL_LINES. Lines lack normals/UVs/
    /// tangents, so switching to Lines also flips the node to unlit and
    /// disables shadow casting (lines can't sensibly shadow anything).
    enum class DrawMode { Triangles, Lines };

    void setDrawMode(DrawMode m);
    DrawMode drawMode() const { return drawMode_; }

    /// Line width in pixels, forwarded to glLineWidth() before the draw.
    /// Most core-profile drivers clamp to 1; values >1 are not guaranteed.
    /// Ignored when drawMode == Triangles.
    void setLineWidth(float w) { lineWidth_ = (w > 0.0f) ? w : 1.0f; }
    float lineWidth() const { return lineWidth_; }

    // --- Mesh data ---

    /// Set mesh geometry. Uploads to GPU on next render and invalidates the
    /// cached BVH (rebuilt lazily on the next raycast).
    void setMesh(const bromesh::MeshData& mesh);
    void setMesh(bromesh::MeshData&& mesh);
    const bromesh::MeshData& mesh() const { return mesh_; }

    /// Local-space AABB of the current mesh. Cached; updated in setMesh.
    /// Returns an empty box for empty meshes.
    const bromath::AABB3& localBounds() const { return bounds_; }

    /// Lazily-built, cached BVH over the current mesh. Built on first call
    /// after setMesh and reused until the next setMesh. Used by scene.raycast
    /// to avoid O(N) ray-triangle tests on dense meshes (terrain chunks etc.).
    const bromesh::MeshBVH& bvh() const;

    // --- Discrete LOD chain (Godot mesh-LOD analog) ---
    // A non-empty chain REPLACES the base mesh for rendering: each frame the
    // level whose maxDist first exceeds the camera distance draws (levels are
    // kept sorted by maxDist ascending; beyond the last maxDist the coarsest
    // level keeps drawing — pair with setVisibilityRange to cull entirely).
    // Distance is measured to the node's world origin, selected once per
    // frame by SceneGraph::updateVisibilityGates(), and shared by EVERY pass
    // — color and shadow draw the same selected level, so a caster can never
    // shadow with a different silhouette than it renders with.
    //
    // The base mesh (setMesh) stays the raycast/BVH source — set it to the
    // highest-detail level if the node must be pickable. Culling bounds are
    // the union of all levels (+ base mesh), so a level switch never pops.
    // Not supported on SkinnedMeshNode (setLodMeshes warns and ignores;
    // per-instance LOD for InstancedMeshNode is likewise out of scope).

    struct LodLevel {
        bromesh::MeshData mesh;
        float maxDist = 0.0f;   // level draws while cameraDist < maxDist
    };

    /// Install (or, with an empty vector, clear) the LOD chain. Copies are
    /// taken by move; tangents are auto-generated per level like setMesh.
    void setLodMeshes(std::vector<LodLevel> levels);

    bool hasLodChain() const { return !lods_.empty(); }
    int lodCount() const { return static_cast<int>(lods_.size()); }

    /// Currently selected chain index (0 when the chain is empty).
    int selectedLod() const { return lodSelected_; }

    /// Per-frame selection from camera distance (SceneGraph's distance
    /// pass). Bumps the change generation on a switch so shadow tiles
    /// holding the old level's silhouette re-render.
    void selectLodByDistance(float d);

    /// True when drawRaw can emit geometry: a non-empty base mesh or a LOD
    /// chain. Pass gathers use this instead of mesh().empty() so chain-only
    /// nodes aren't skipped.
    bool hasDrawableMesh() const { return !mesh_.empty() || !lods_.empty(); }

    // --- Material ---

    void setColor(float r, float g, float b, float a = 1.0f) {
        color_[0] = r; color_[1] = g; color_[2] = b; color_[3] = a;
    }
    const float* color() const { return color_; }

    /// Whether the current mesh has per-vertex colors (set after uploadToGPU).
    bool hasVertexColors() const { return hasVertexColors_; }

    /// Whether the per-vertex color stream tints the albedo. Tri-state:
    /// -1 (default) auto — tint iff the mesh has a color buffer, the
    /// historical behavior. 0/1 explicitly force it off/on. Forcing it off
    /// lets a mesh carry a color buffer purely for the wind-bend channel
    /// (vertex color R, read independently by the wind VS) without the
    /// remaining channels washing the albedo — so foliage sways from wind
    /// yet keeps its flat/material/textured colour instead of tinting red.
    void setVertexColorTint(bool b) { vertexColorTint_ = b ? 1 : 0; }
    bool vertexColorTintEnabled() const {
        return (vertexColorTint_ < 0) ? hasVertexColors_
                                      : (vertexColorTint_ != 0 && hasVertexColors_);
    }

    /// Upload an RGBA8 baseColor texture (tightly packed, top-left origin).
    /// Pass width=0 / height=0 / data=nullptr to clear. Takes a copy; the
    /// caller's buffer can be freed immediately after.
    void setBaseColorTexture(int width, int height, const uint8_t* rgba);
    void clearBaseColorTexture();
    bool hasBaseColorTexture() const {
        return externalBaseColorTex_ != nullptr || texture_ != 0;
    }
    GLuint baseColorTextureId() const { return texture_; }

    /// Live-linked external baseColor texture (scene-as-texture). The
    /// provider is invoked at draw time every frame and returns the CURRENT
    /// GL texture name to sample — or 0 when the source has nothing yet
    /// (never rendered) or no longer exists (source scene destroyed), in
    /// which case the mesh draws its plain base color. Per-draw resolution
    /// is what makes the link live: FBO textures are recreated on canvas
    /// resize / renderScale changes, and the new id is picked up on the next
    /// frame with no re-wiring. MeshNode never owns or deletes an external
    /// texture (releaseGL ignores it). Mutually exclusive with the owned
    /// setBaseColorTexture(bytes) path — setting either clears the other.
    using ExternalTextureProvider = std::function<unsigned()>;
    void setExternalBaseColorTexture(ExternalTextureProvider provider);
    bool hasExternalBaseColorTexture() const {
        return externalBaseColorTex_ != nullptr;
    }

    /// Draw-time baseColor resolution: the external provider when set (may
    /// return 0 — see above), else the owned texture (0 if none).
    GLuint resolvedBaseColorTextureId() const {
        return externalBaseColorTex_ ? (GLuint)externalBaseColorTex_()
                                     : texture_;
    }

    /// Tangent-space normal map (RGBA8, .xy = xy, .z ignored and reconstructed,
    /// or full .xyz sampled directly — this shader reads all three channels).
    void setNormalTexture(int width, int height, const uint8_t* rgba);
    void clearNormalTexture();
    bool hasNormalTexture() const { return normalTex_ != 0; }
    GLuint normalTextureId() const { return normalTex_; }

    /// Metallic-roughness texture (glTF packing: G = roughness, B = metallic).
    /// R and A are unused. Scalar metallic/roughness multiply with the sample.
    void setMetallicRoughnessTexture(int width, int height, const uint8_t* rgba);
    void clearMetallicRoughnessTexture();
    bool hasMetallicRoughnessTexture() const { return mrTex_ != 0; }
    GLuint metallicRoughnessTextureId() const { return mrTex_; }

    /// Ambient-occlusion map (R channel used). Modulates ambient/IBL only.
    void setOcclusionTexture(int width, int height, const uint8_t* rgba);
    void clearOcclusionTexture();
    bool hasOcclusionTexture() const { return aoTex_ != 0; }
    GLuint occlusionTextureId() const { return aoTex_; }

    /// Emissive map (RGB). Multiplied by the scalar `emissive` and the
    /// `emissiveColor` tint — set intensity=1 and color to the glTF
    /// emissiveFactor to match glTF's `emissiveTexture * emissiveFactor`.
    void setEmissiveTexture(int width, int height, const uint8_t* rgba);
    void clearEmissiveTexture();
    bool hasEmissiveTexture() const { return emissiveTex_ != 0; }
    GLuint emissiveTextureId() const { return emissiveTex_; }

    void setEmissive(float e) { emissive_ = e; }
    float emissive() const { return emissive_; }

    /// Unlit mode: skip lighting (no light loop, no ambient, no PBR BRDF).
    /// Output is baseColor + emissiveColor*emissive (fog/nearClip still apply).
    /// Used for overlay meshes that shouldn't receive scene lighting — e.g.
    /// editor gizmo handles — so their appearance is independent of the
    /// lighting rig the app has set up.
    void setUnlit(bool u) { unlit_ = u; }
    bool unlit() const { return unlit_; }

    /// The unlit flag as the renderer applies it: a custom shader suppresses
    /// unlit (its early-return would skip the userFragment hook, and unlit
    /// meshes draw post-tonemap where the hook's PBR inputs don't exist).
    /// Every pass — color routing, uUnlit upload, shadow caster gather —
    /// keys off this so they can never disagree.
    bool effectiveUnlit() const { return unlit_ && !customShader_; }

    // --- PBR material ---
    // baseColor is the existing color_[4] (RGB used as linear albedo, A as
    // mesh transparency). metallic/roughness follow the glTF convention:
    //   metallic 0 = dielectric (plastic/wood/skin), 1 = metal.
    //   roughness 0 = mirror, 1 = fully diffuse.
    // emissiveColor is a separate linear-RGB tint multiplied by the scalar
    // `emissive` intensity; set both to get self-lit surfaces.

    void setMetallic(float m) { metallic_ = m; }
    float metallic() const { return metallic_; }

    void setRoughness(float r) { roughness_ = r; }
    float roughness() const { return roughness_; }

    void setEmissiveColor(float r, float g, float b) {
        emissiveColor_[0] = r; emissiveColor_[1] = g; emissiveColor_[2] = b;
    }
    const float* emissiveColor() const { return emissiveColor_; }

    /// Polygon offset (forwarded to glPolygonOffset before drawing this mesh).
    /// Negative `units` pulls the surface forward in the depth buffer, useful
    /// for layering co-located meshes (e.g. high-detail LOD meshes that should
    /// always win the depth test against lower-detail backdrops).
    /// Set both to 0 to disable.
    void setDepthBias(float factor, float units) {
        depthBiasFactor_ = factor;
        depthBiasUnits_ = units;
    }
    float depthBiasFactor() const { return depthBiasFactor_; }
    float depthBiasUnits() const { return depthBiasUnits_; }

    /// Near clip distance — fragments closer than this are discarded.
    /// Used to hide coarse LOD terrain where finer LODs are loaded.
    void setNearClipDist(float d) { nearClipDist_ = d; }
    float nearClipDist() const { return nearClipDist_; }

    /// Whether this mesh writes into the shadow atlas. Default true. Set false
    /// for meshes that should illuminate with shadows but not cast their own
    /// (foliage impostors, transparent geometry, debug markers).
    void setCastsShadow(bool b) { castsShadow_ = b; }
    bool castsShadow() const { return castsShadow_; }

    /// Whether light contributions to this mesh are shadow-attenuated. Default
    /// true. Set false for geometry that should stay fully lit regardless of
    /// occluders (self-lit props, UI billboards in the 3D world).
    void setReceivesShadow(bool b) { receivesShadow_ = b; }
    bool receivesShadow() const { return receivesShadow_; }

    /// Disable backface culling for this mesh (leaves, paper, fabric — anything
    /// alpha-cut where you want to see both sides). Defaults to false.
    void setTwoSided(bool b) { twoSided_ = b; }
    bool twoSided() const { return twoSided_; }

    /// Subsurface / wrap-lighting amount [0,1]. When > 0 AND twoSided is on,
    /// the shader adds a cheap leaf-translucency term that lights the back
    /// of the surface relative to the dominant directional light.
    void setSubsurface(float s) { subsurface_ = s; }
    float subsurface() const { return subsurface_; }

    /// Alpha cutoff threshold. 0 (default) disables. When >0, fragments with
    /// baseAlpha (texture.a * uColor.a, or vColor.a) below this threshold
    /// are discarded — for cut-out leaves/petals using straight-alpha textures.
    void setAlphaCutoff(float c) { alphaCutoff_ = c; }
    float alphaCutoff() const { return alphaCutoff_; }

    /// Wind sway opt-in [0,1]. The mesh VS multiplies the per-vertex bend
    /// (sourced from vertex color R, 0..1) by this scalar before applying
    /// the global wind delta. Defaults to 0 (no sway) so terrain and other
    /// vertex-coloured meshes don't ripple. Flora meshes set this to 1.
    void setWindMask(float m) { windMask_ = m; }
    float windMask() const { return windMask_; }

    // --- Custom shader ---
    // See custom_shader.h for the shared state struct (same surface exists
    // on InstancedMeshNode). Works on static AND skinned meshes — the
    // renderer picks the SKINNED program variant for a ready skin.

    /// Install user shader chunks (either may be empty, not both — callers
    /// validate). Replaces any previous shader; uniform values reset.
    /// Bumps the change generation: a vertex chunk changes the shadow
    /// silhouette, and clearing one must invalidate tiles that still hold
    /// the displaced depth.
    void setCustomShader(std::string vertexChunk, std::string fragmentChunk) {
        customShader_ = CustomShaderState::make(std::move(vertexChunk),
                                                std::move(fragmentChunk));
        bumpChangeGeneration();
    }
    void clearCustomShader() {
        customShader_.reset();
        bumpChangeGeneration();
    }
    bool hasCustomShader() const { return customShader_ != nullptr; }
    const CustomShaderState* customShader() const { return customShader_.get(); }

    /// Set (or update) a numeric user-uniform value on this node. No-op
    /// without a custom shader installed.
    void setCustomShaderUniform(const std::string& name, int comps,
                                const float* vals) {
        if (customShader_) {
            customShader_->setUniform(name, comps, vals);
            bumpChangeGeneration();
        }
    }

    // --- Custom-shader sampler uniforms ---
    // Single-channel float (R32F) textures a user fragment/vertex chunk can
    // sample, e.g. a terrain heightfield raymarched by a sky-dome shader.
    //
    // Texture units: the mesh uber-shader already owns units 0..9 (baseColor
    // 0, shadow atlas 1, IBL irradiance/prefilter/BRDF 2/3/4, normal 5, MR 6,
    // AO 7, emissive 8, reflection probe 9). User samplers therefore start at
    // unit 10, and a node may bind whatever is left above that.
    //
    // The limit is QUERIED, not assumed. It used to be a constexpr 16 — GL
    // 3.3's guaranteed minimum — which left exactly 6 user slots on every
    // machine regardless of what the machine had. That is a real budget on
    // hardware from 2010 and pure invention on anything since: desktop drivers
    // in practice report 32 to 192. A terrain wanting four height layers, a
    // control-channel layer and a water layer hits 6 exactly, and the next
    // feature that needs a sampler is then blocked by a number the hardware
    // never imposed.
    //
    // The floor is unchanged: render::GLCaps clamps up to 16 and returns 16
    // before the latch, so nothing that worked under the old constant can stop
    // working under this. See render/gl_context.h.
    static constexpr int kUserTextureUnitBase = 10;
    static int userTextureUnitLimit();
    static int maxUserTextures();

    /// One user sampler slot. `data`/`w`/`h` stage a CPU-side upload that
    /// flushPendingTextures() consumes on the GL thread; `tex` is the owned
    /// GL name (0 until first upload / after release).
    struct UserTexture {
        /// A staged sub-rectangle write (glTexSubImage2D at the next flush).
        /// Kept as a queue rather than folded into `data` because the slot
        /// does not keep a CPU mirror of the texture after upload — there is
        /// nothing to fold into.
        struct SubUpdate {
            std::vector<float> data;
            int x = 0, y = 0, w = 0, h = 0;
        };

        std::string name;            // carries the `u_` prefix
        std::vector<float> data;     // staged R32F pixels (cleared on upload)
        int w = 0;
        int h = 0;
        int channels = 1;
        bool dirty = false;
        // Generate a mip chain and use trilinear minification. Off by default:
        // a heightfield raymarcher wants the level-0 samples it staged, and
        // the chain costs both memory and a per-upload generate pass.
        bool mipmap = false;
        // GL_REPEAT instead of GL_CLAMP_TO_EDGE. Off by default: a slot is
        // usually a window onto the world with no meaning outside its extent,
        // where repeating would wrap the far edge into view. A slot that is a
        // TILE — one periodic patch sampled at arbitrary coordinates — needs
        // the opposite, and cannot get it from a fract() in the shader: that
        // leaves a texel-wide seam at the wrap which every mip level widens.
        bool repeat = false;
        // Repeat in S but CLAMP in T. An equirectangular chart is periodic in
        // longitude and emphatically not in latitude: wrapping T blends the
        // north pole row into the south pole row across the half texel past
        // v = 0, which puts the wrong hemisphere's elevation within a few km of
        // each pole. Clamping there is not an approximation — the pole rows are
        // single-valued, so the clamped value is the correct one.
        bool clampT = false;
        GLuint tex = 0;
        std::vector<SubUpdate> subUpdates;
    };

    /// Stage a float texture for the named user sampler.
    /// `data` must hold width*height*channels floats; pass data=nullptr (or a zero
    /// extent) to release the slot. `mipmap` opts the slot into a generated
    /// mip chain with GL_LINEAR_MIPMAP_LINEAR minification, which is what a
    /// shader sampling textureLod() at a FRACTIONAL level needs — without it
    /// GL has no second level to blend toward and every level reads as 0.
    /// Returns false only when a NEW name would exceed maxUserTextures() —
    /// existing names always succeed. Safe off the GL thread: the upload
    /// happens in flushPendingTextures().
    bool setCustomShaderTexture(const std::string& name, int width, int height,
                                const float* data, bool mipmap = false,
                                bool repeat = false, bool clampT = false,
                                int channels = 1);

    /// Stage a sub-rectangle write into an EXISTING slot, avoiding the
    /// reallocation (and mip-chain rebuild from scratch) a full re-upload
    /// costs. `data` must hold width*height floats laid out row-major for the
    /// sub-rect alone. Returns false — logging, never writing partially or
    /// out of bounds — when the slot is unknown, has no dimensions yet, or
    /// the rect falls outside them. Safe off the GL thread; bounds are
    /// checked against the CPU-side extent, which outlives the staged bytes.
    bool updateCustomShaderTexture(const std::string& name, int x, int y,
                                   int width, int height, const float* data);
    /// Release the named slot (GL delete happens at the next flush).
    void clearCustomShaderTexture(const std::string& name);
    const std::vector<UserTexture>& customShaderTextures() const {
        return userTextures_;
    }

    // --- Culling margin ---
    // Extra world-space padding (in units) added to this node's frustum- and
    // shadow-culling bounds. A custom vertex shader that displaces geometry
    // beyond the mesh's AABB can otherwise be culled while still visible —
    // set this to the maximum displacement (Godot's extra_cull_margin has
    // the same contract). 0 by default.
    void setCullMargin(float m) { cullMargin_ = m < 0.0f ? 0.0f : m; }
    float cullMargin() const { return cullMargin_; }

    /// Upload/release any dirty staged texture slots. GL thread only. The
    /// renderer calls this before reading material texture state so runtime
    /// texture swaps (setBaseColorTexture and friends) apply the same frame
    /// they were set; drawRaw also flushes for depth-only paths.
    ///
    /// Two calls land on every mesh draw (renderMeshNode, then drawRaw), and on
    /// a steady scene both have nothing to do — but the work they skip used to
    /// be behind an out-of-line call that ran a remove_if over userTextures_
    /// regardless. The gate is derived from the staged state rather than kept
    /// as a flag, so no future setter can forget to raise it.
    void flushPendingTextures() {
        if (pendingBase_.dirty || pendingNormal_.dirty || pendingMR_.dirty ||
            pendingAO_.dirty || pendingEmissive_.dirty || !userTextures_.empty())
            flushPendingTexturesImpl();
    }

    /// Release GPU resources (call before GL context is destroyed).
    virtual void releaseGL();

    /// Bind VAO and issue glDrawElements without touching material uniforms.
    /// Used by depth-only passes (shadow maps) where the caller's program is
    /// already bound and only positions matter. Returns true if anything drew.
    bool drawRaw();

    // Staged texture upload — setters capture the bytes and uploadToGPU()
    // (which runs in the render thread with GL context bound) actually uploads.
    // Public so file-scope helpers in mesh_node.cpp can operate on slots.
    struct PendingTex {
        std::vector<uint8_t> data;
        int w = 0;
        int h = 0;
        bool dirty = false;
    };

protected:
    /// Interleave + upload the vertex/index buffers (GL thread, called from
    /// drawRaw when gpuDirty_). Virtual so SkinnedMeshNode can append its
    /// joint/weight attribute streams to the same VAO.
    virtual void uploadToGPU();

    // GL vertex-array handle — shared with SkinnedMeshNode's skin-attribute
    // upload, which binds it to add attributes 5/6.
    GLuint vao_ = 0;

public:
    /// Inverse-transpose of `model`'s upper 3x3 — the normal matrix, so normals
    /// stay perpendicular under non-uniform scale. Returns 9 floats in column-
    /// major order, valid until the next call on this node.
    ///
    /// Cached against the 3x3 it was built from, because the alternative is a
    /// full 4x4 inverse + transpose on every draw. The per-frame camera-relative
    /// offset the renderer applies touches only column 3, which drops out of the
    /// 3x3 entirely — so a mesh that is not rotating or scaling computes this
    /// once and then never again, however far the camera travels.
    const float* normalMatrix3(const bromath::Mat4& model) const;

private:
    /// The real flush. Only reached when flushPendingTextures() sees staged
    /// work; it is also the one place that can rebind a texture out from under
    /// a caller, which is why the gate above matters beyond speed.
    void flushPendingTexturesImpl();

    mutable float normalMat3_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    mutable float normalMatSrc_[9] = {};   // the 3x3 normalMat3_ was built from
    mutable bool  normalMatValid_ = false;

    // Recompute bounds_ from the base mesh + every LOD level (union), so
    // culling stays valid across level switches.
    void recomputeBounds();

    bromesh::MeshData mesh_;
    bool gpuDirty_ = false;

    // Cached bounds + BVH. Both are invalidated (bvhDirty_ = true, bounds_
    // recomputed) on every setMesh call. The BVH covers the BASE mesh only;
    // bounds_ additionally unions the LOD chain.
    bromath::AABB3 bounds_;
    mutable bromesh::MeshBVH bvh_;
    mutable bool bvhDirty_ = true;

    // LOD chain (sorted by maxDist ascending). Each level owns its own GL
    // buffer set, uploaded lazily on first draw; replaced levels stage their
    // GL names into the dead lists, deleted at the next draw / releaseGL on
    // the GL thread (setters may run without a context current, matching the
    // staged-texture pattern above).
    struct LodEntry {
        bromesh::MeshData mesh;
        float maxDist = 0.0f;
        GLuint vao = 0, vbo = 0, ibo = 0;
        GLsizei indexCount = 0;
        bool gpuDirty = true;
        bool hasColors = false;
    };
    std::vector<LodEntry> lods_;
    int lodSelected_ = 0;
    std::vector<GLuint> deadLodVaos_;
    std::vector<GLuint> deadLodBufs_;
    void flushDeadLodBuffers();   // GL thread

    // GL resources
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLuint texture_ = 0;
    GLuint normalTex_ = 0;
    GLuint mrTex_ = 0;
    GLuint aoTex_ = 0;
    GLuint emissiveTex_ = 0;
    GLsizei indexCount_ = 0;

    // Live-linked external baseColor source (see setExternalBaseColorTexture).
    // Never a GL name we own — releaseGL must not (and cannot) delete it.
    ExternalTextureProvider externalBaseColorTex_;

    PendingTex pendingBase_;
    PendingTex pendingNormal_;
    PendingTex pendingMR_;
    PendingTex pendingAO_;
    PendingTex pendingEmissive_;

    // Material
    bool hasVertexColors_ = false;
    // Albedo vertex-color tint: -1 auto (tint iff color buffer present),
    // 0 forced off, 1 forced on. See setVertexColorTint.
    int  vertexColorTint_ = -1;
    float color_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive_ = 0.0f;
    float emissiveColor_[3] = {1.0f, 1.0f, 1.0f};
    float metallic_ = 0.0f;
    float roughness_ = 0.7f;
    bool  unlit_ = false;

    // Polygon offset (per-mesh depth bias for layered LOD meshes)
    float depthBiasFactor_ = 0.0f;
    float depthBiasUnits_ = 0.0f;

    // Near clip distance — discard fragments closer than this (for LOD overlap)
    float nearClipDist_ = 0.0f;

    bool castsShadow_ = true;
    bool receivesShadow_ = true;
    bool twoSided_ = false;
    float subsurface_ = 0.0f;
    float alphaCutoff_ = 0.0f;
    float windMask_ = 0.0f;

    DrawMode drawMode_ = DrawMode::Triangles;
    float lineWidth_ = 1.0f;

    // Custom shader chunks + user-uniform values (null = default pipeline).
    // Heap-allocated so the common shaderless mesh pays one pointer.
    std::unique_ptr<CustomShaderState> customShader_;
    // User sampler slots (see setCustomShaderTexture). Deliberately NOT part
    // of CustomShaderState: setShader() replaces that state wholesale, which
    // would drop owned GL names on the floor. Owned here, released in
    // releaseGL() alongside the material textures.
    std::vector<UserTexture> userTextures_;
    float cullMargin_ = 0.0f;
};

} // namespace bro::scene
