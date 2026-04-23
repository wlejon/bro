#pragma once

#include "scene/scene_node.h"
#include <bromesh/mesh_data.h>
#include <bromesh/analysis/bvh.h>
#include <glad/gl.h>

#include <vector>

namespace bro::scene {

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

    // --- Mesh data ---

    /// Set mesh geometry. Uploads to GPU on next render and invalidates the
    /// cached BVH (rebuilt lazily on the next raycast).
    void setMesh(const bromesh::MeshData& mesh);
    void setMesh(bromesh::MeshData&& mesh);
    const bromesh::MeshData& mesh() const { return mesh_; }

    /// Local-space AABB of the current mesh. Cached; updated in setMesh.
    /// Returns an empty box for empty meshes.
    const bromesh::BBox& localBounds() const { return bounds_; }

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

    /// Upload an RGBA8 baseColor texture (tightly packed, top-left origin).
    /// Pass width=0 / height=0 / data=nullptr to clear. Takes a copy; the
    /// caller's buffer can be freed immediately after.
    void setBaseColorTexture(int width, int height, const uint8_t* rgba);
    void clearBaseColorTexture();
    bool hasBaseColorTexture() const { return texture_ != 0; }
    GLuint baseColorTextureId() const { return texture_; }

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

    /// Release GPU resources (call before GL context is destroyed).
    void releaseGL();

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

private:
    void uploadToGPU();

    bromesh::MeshData mesh_;
    bool gpuDirty_ = false;

    // Cached bounds + BVH. Both are invalidated (bvhDirty_ = true, bounds_
    // recomputed) on every setMesh call.
    bromesh::BBox bounds_;
    mutable bromesh::MeshBVH bvh_;
    mutable bool bvhDirty_ = true;

    // GL resources
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ibo_ = 0;
    GLuint texture_ = 0;
    GLuint normalTex_ = 0;
    GLuint mrTex_ = 0;
    GLuint aoTex_ = 0;
    GLuint emissiveTex_ = 0;
    GLsizei indexCount_ = 0;

    PendingTex pendingBase_;
    PendingTex pendingNormal_;
    PendingTex pendingMR_;
    PendingTex pendingAO_;
    PendingTex pendingEmissive_;

    // Material
    bool hasVertexColors_ = false;
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
};

} // namespace bro::scene
