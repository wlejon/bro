#pragma once

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

    // --- Custom shader (static meshes only for now) ---
    // User GLSL chunks spliced into the mesh uber-shader (see the
    // //__USER_CHUNK__ markers in mesh.vert / mesh.frag). The node stores
    // only sources + numeric uniform values — the compiled programs live in
    // SceneRenderer's cache, keyed by `key`, so identical sources across
    // nodes share one program. Compilation/validation happens in the
    // renderer (SceneGraph::compileCustomShader) BEFORE this state is set,
    // so a node with custom-shader state always maps to a linked program.

    struct CustomShaderUniform {
        std::string name;      // must carry the `u_` user-namespace prefix
        int comps = 1;         // 1..4 → float / vec2 / vec3 / vec4
        float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct CustomShaderState {
        std::string vertexChunk;
        std::string fragmentChunk;
        std::string key;       // program-cache key: vertex + '\x1f' + fragment
        std::vector<CustomShaderUniform> uniforms;
    };

    /// Install user shader chunks (either may be empty, not both — callers
    /// validate). Replaces any previous shader; uniform values reset.
    void setCustomShader(std::string vertexChunk, std::string fragmentChunk) {
        auto st = std::make_unique<CustomShaderState>();
        st->key = vertexChunk + '\x1f' + fragmentChunk;
        st->vertexChunk = std::move(vertexChunk);
        st->fragmentChunk = std::move(fragmentChunk);
        customShader_ = std::move(st);
    }
    void clearCustomShader() { customShader_.reset(); }
    bool hasCustomShader() const { return customShader_ != nullptr; }
    const CustomShaderState* customShader() const { return customShader_.get(); }

    /// Set (or update) a numeric user-uniform value on this node. Values are
    /// plain floats — nothing JS-owned — and are uploaded per draw, so two
    /// nodes sharing a program can carry different values. No-op without a
    /// custom shader installed.
    void setCustomShaderUniform(const std::string& name, int comps,
                                const float* vals) {
        if (!customShader_) return;
        if (comps < 1) comps = 1;
        if (comps > 4) comps = 4;
        for (auto& u : customShader_->uniforms) {
            if (u.name == name) {
                u.comps = comps;
                for (int i = 0; i < comps; ++i) u.v[i] = vals[i];
                return;
            }
        }
        CustomShaderUniform u;
        u.name = name;
        u.comps = comps;
        for (int i = 0; i < comps; ++i) u.v[i] = vals[i];
        customShader_->uniforms.push_back(std::move(u));
    }

    /// Upload/release any dirty staged texture slots. GL thread only. The
    /// renderer calls this before reading material texture state so runtime
    /// texture swaps (setBaseColorTexture and friends) apply the same frame
    /// they were set; drawRaw also flushes for depth-only paths.
    void flushPendingTextures();

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

private:
    bromesh::MeshData mesh_;
    bool gpuDirty_ = false;

    // Cached bounds + BVH. Both are invalidated (bvhDirty_ = true, bounds_
    // recomputed) on every setMesh call.
    bromath::AABB3 bounds_;
    mutable bromesh::MeshBVH bvh_;
    mutable bool bvhDirty_ = true;

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
};

} // namespace bro::scene
