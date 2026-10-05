#pragma once

#include "scene/custom_shader.h"
#include "scene/scene_node.h"
#include "scene/shade_map.h"
#include <bromath/aabb.h>
#include <bromesh/mesh_data.h>
#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/bvh.h>
#include "scene/texture_source.h"

#include <vector>
#include <cstdint>

namespace bro::scene {

/// Renders many copies of one mesh in a single instanced draw.
/// One mesh + one material is shared across all instances; per-instance state is a
/// 4x3 affine model transform plus an RGBA color tint, packed as 16 floats:
///
///   [m00 m01 m02 tx]
///   [m10 m11 m12 ty]
///   [m20 m21 m22 tz]
///   [r   g   b   a ]
///
/// The instance color is multiplied with the material baseColor in the fragment
/// shader. The alpha channel is reserved for future per-instance atlas indexing.
class InstancedMeshNode : public SceneNode {
public:
    explicit InstancedMeshNode(const std::string& name = "");
    ~InstancedMeshNode() override = default;

    InstancedMeshNode(const InstancedMeshNode&) = delete;
    InstancedMeshNode& operator=(const InstancedMeshNode&) = delete;

    Type type() const override { return Type::InstancedMesh; }

    // --- Mesh ---

    void setMesh(const bromesh::MeshData& mesh);
    void setMesh(bromesh::MeshData&& mesh);
    const bromesh::MeshData& mesh() const { return mesh_; }
    const bromath::AABB3& localBounds() const { return bounds_; }

    // --- Instances ---

    /// Set the per-instance buffer directly. `data` must point at exactly
    /// `count * 16` floats in the canonical layout. Bytes are copied; the
    /// caller's buffer can be freed immediately after this returns.
    void setInstances(const float* data, size_t count);

    /// Convenience: build the instance buffer from a 9-floats-per-instance
    /// (px py pz, qx qy qz qw, scale, variantIndex) array. variantIndex is
    /// packed into color.a (0..255 → 0..1); RGB defaults to white. Color
    /// can be overridden per-instance later via updateInstance().
    void setInstancesFromPosQuatScale(const float* data, size_t count);

    /// Replace one instance's 16-float record in place.
    void updateInstance(size_t i, const float* data16);

    size_t instanceCount() const { return instanceCount_; }

    /// Decompose instance `i` into its affine rows: `outRows` receives 12
    /// floats (m00 m01 m02 tx, m10 …). False when `i` is out of range.
    bool instanceRows(size_t i, float* outRows) const {
        if (i >= instanceCount_ || instanceData_.size() < (i + 1) * 16) return false;
        const float* rec = instanceData_.data() + i * 16;
        for (int k = 0; k < 12; ++k) outRows[k] = rec[k];
        return true;
    }

    /// Instance `i`'s whole 16-float record: the 12 affine rows, then the
    /// RGBA tint. False when `i` is out of range.
    bool instanceRecord(size_t i, float* out16) const {
        if (i >= instanceCount_ || instanceData_.size() < (i + 1) * 16) return false;
        const float* rec = instanceData_.data() + i * 16;
        for (int k = 0; k < 16; ++k) out16[k] = rec[k];
        return true;
    }

    const std::vector<float>& instanceData() const { return instanceData_; }

    /// Moves whenever the mesh changes (the renderer re-uploads geometry).
    uint64_t geometryGeneration() const { return geometryGeneration_; }

    /// Lazily-built, cached BVH over the instance mesh — one BVH shared by every
    /// instance, since they all draw the same geometry. Mirrors MeshNode::bvh().
    const bromesh::MeshBVH& bvh() const;

    // --- Material (mirrors MeshNode) ---

    void setColor(float r, float g, float b, float a = 1.0f) {
        color_[0] = r; color_[1] = g; color_[2] = b; color_[3] = a;
        emissiveColor_[0] = r; emissiveColor_[1] = g; emissiveColor_[2] = b;
    }
    const float* color() const { return color_; }

    // Material textures: tightly packed RGBA8, copied. The renderer uploads
    // a slot when its generation moves (see texture_source.h).
    void setBaseColorTexture(int width, int height, const uint8_t* rgba);
    void clearBaseColorTexture();
    bool hasBaseColorTexture() const { return !baseColorTex_.empty(); }
    const NodeTexture& baseColorTexture() const { return baseColorTex_; }

    void setNormalTexture(int width, int height, const uint8_t* rgba);
    void clearNormalTexture();
    bool hasNormalTexture() const { return !normalTex_.empty(); }
    const NodeTexture& normalTexture() const { return normalTex_; }

    void setMetallicRoughnessTexture(int width, int height, const uint8_t* rgba);
    void clearMetallicRoughnessTexture();
    bool hasMetallicRoughnessTexture() const { return !mrTex_.empty(); }
    const NodeTexture& metallicRoughnessTexture() const { return mrTex_; }

    void setOcclusionTexture(int width, int height, const uint8_t* rgba);
    void clearOcclusionTexture();
    bool hasOcclusionTexture() const { return !aoTex_.empty(); }
    const NodeTexture& occlusionTexture() const { return aoTex_; }

    void setEmissiveTexture(int width, int height, const uint8_t* rgba);
    void clearEmissiveTexture();
    bool hasEmissiveTexture() const { return !emissiveTex_.empty(); }
    const NodeTexture& emissiveTexture() const { return emissiveTex_; }

    void setEmissive(float e) { emissive_ = e; }
    float emissive() const { return emissive_; }

    void setUnlit(bool u) { unlit_ = u; }
    bool unlit() const { return unlit_; }

    /// Unlit as the renderer applies it — a custom shader suppresses unlit;
    /// same contract as MeshNode::effectiveUnlit.
    bool effectiveUnlit() const { return unlit_ && !customShader_; }

    /// Alpha-test cutoff. > 0 enables `discard` for fragments whose final
    /// alpha is below the threshold (used for leaf cards and similar
    /// cutout textures). 0 disables the test.
    void  setAlphaCutoff(float c) { alphaCutoff_ = c; }
    float alphaCutoff() const { return alphaCutoff_; }

    /// Disable back-face culling for this node. Required for double-sided
    /// foliage so the back face of a leaf card is also visible.
    void setDoubleSided(bool b) { doubleSided_ = b; }
    bool doubleSided() const { return doubleSided_; }

    /// Shade map (see shade_map.h): same contract as MeshNode::setShadeMap.
    void setShadeMap(ShadeMapProvider p) { shadeMap_ = std::move(p); }
    void clearShadeMap() { shadeMap_ = nullptr; }
    const ShadeMapProvider* shadeMap() const {
        return shadeMap_ ? &shadeMap_ : nullptr;
    }

    // --- Static batching ---
    // Collapse ALL instances into ONE merged mesh drawn as a single instance.
    // An instanced draw carries a fixed ~7-9us of GPU time PER INSTANCE
    // on some drivers (measured + Nsight-confirmed), independent of the
    // instance's geometry — negligible for a big mesh drawn a few times, but
    // catastrophic for high counts of tiny (few-triangle) meshes (20000 quads =
    // ~150ms vs ~0.03ms merged). Static batch bakes each instance's transform
    // into vertices, its RGB tint into vertex colors, and its atlas cell into
    // UVs, then draws the merge as instanceCount=1 through the SAME shader, so
    // it renders pixel-identical while paying the per-instance cost exactly
    // once. Trade-off: merged geometry costs memory, and any instance/mesh
    // change forces an O(total-verts) CPU rebake on the next draw — only worth
    // it when instances rarely move and each mesh is small. Ignored while a
    // custom shader is set (camera-facing billboards can't bake a static
    // transform — they use a shader-side merge instead). Atlas baking assumes a
    // base-colour-only material (normal/MR/AO/emissive keep raw UVs in the
    // shader, so they'd mis-sample a baked atlas sub-rect).
    void setStaticBatch(bool b);
    bool staticBatch() const { return staticBatch_; }

    /// True when the NEXT draw renders as one merged instance: the flag is on,
    /// there is geometry to merge, and no custom shader overrides it. The
    /// renderer keys the atlas + vertex-colour-tint uniforms off this.
    bool renderingBatched() const {
        return staticBatch_ && !customShader_ && instanceCount_ > 0 && !mesh_.empty();
    }
    /// Atlas grid the current draw samples: 1x1 when batched (the cell is baked
    /// into the merged UVs so the shader must NOT remap), else the authored grid.
    /// The merged geometry a batched draw renders (renderingBatched()),
    /// rebaked here when an instance, the mesh or the atlas changed since.
    const bromesh::MeshData& staticBatchMesh() const {
        if (batchDirty_) rebuildStaticBatch();
        return batchMesh_;
    }
    /// Changes whenever staticBatchMesh() is rebaked (GPU cache key).
    uint64_t staticBatchGeneration() const { return batchGeneration_; }
    int effectiveAtlasCols() const { return renderingBatched() ? 1 : atlasCols_; }
    int effectiveAtlasRows() const { return renderingBatched() ? 1 : atlasRows_; }
    /// Whether the draw applies per-vertex colour as albedo: a batched draw
    /// bakes the per-instance tint into vertex colours and always needs it on.
    bool useVertexColorForDraw() const {
        return renderingBatched() ? batchMesh_.hasColors() : vertexColorTintEnabled();
    }

    // --- GPU foliage scatter mode ---
    // Expand `segCount` branch segments into leaves entirely in the vertex
    // shader (vulkan/shaders/mesh_scatter.vert), so the tens-of-thousands-of-leaves
    // scatter never touches the CPU and no per-leaf instance buffer is built or
    // uploaded — only the compact per-segment buffer is. The draw issues
    // `segCount * maxPerSeg` instances; slots past a segment's leaf count clip
    // to a degenerate triangle. `segData` is segCount*8 floats, 2 RGBA32F
    // texels per segment: [from.xyz, radius] then [dir.xyz, leafCount], with
    // dir = to - from and leafCount the CPU-precomputed integer leaf count.
    // Mirrors bromesh::placeLeavesOnBranches; obstacle avoidance / dedup are not
    // modelled (use the CPU path for those). Sets the node into scatter mode —
    // setInstances() and scatter are mutually exclusive. The draw issues one
    // instance per leaf; `instSeg` (instCount entries) maps each leaf to its
    // segment index, so there are no wasted/degenerate instances.
    struct ScatterParams {
        uint32_t seed = 0;
        float    upBias = 0.5f, tiltJitter = 0.3f, rollJitter = 0.2f;
        float    baseScale = 1.0f, scaleJitter = 0.2f, scaleByRadius = 0.0f;
        float    refRadius = 0.05f, densityFalloff = 0.0f;
    };
    void setScatterSegments(const float* segData, size_t segCount,
                            const float* instSeg, size_t instCount,
                            const ScatterParams& params,
                            const float boundsMin[3], const float boundsMax[3]);
    bool isScatter() const { return scatterMode_; }
    size_t scatterSegCount() const { return scatterSegCount_; }
    size_t scatterInstanceCount() const { return scatterInstCount_; }
    const ScatterParams& scatterParams() const { return scatterParams_; }
    const std::vector<float>& scatterSegments() const { return scatterData_; }
    const std::vector<float>& scatterInstanceSegments() const { return scatterInstSeg_; }

    // --- GPU procedural branch-tube mode ---
    // Synthesise tapered tube (stem) geometry entirely in the vertex shader
    // (vulkan/shaders/mesh_tube.vert) from a compact per-segment storage buffer, so
    // a growing skeleton re-uploads only the segment records — never a re-baked
    // multi-MB merged mesh. `segData` is segCount*8 floats, 2 RGBA32F texels per
    // segment: [from.xyz, radiusFrom] then [to.xyz, radiusTo]. Each segment
    // draws `sides` quads (a capless tube wall); the draw issues
    // segCount*sides*6 vertices with NO vertex or instance attributes (the VS
    // reads the vertex index + the segment records). Mutually exclusive with
    // mesh instances and with scatter mode. Casts shadows through
    // shadow_tube.vert and receives them like any lit mesh.
    void setTubeSegments(const float* segData, size_t segCount, int sides,
                         float radiusScale,
                         const float boundsMin[3], const float boundsMax[3]);
    bool isTube() const { return tubeMode_; }
    size_t tubeSegCount() const { return tubeSegCount_; }
    int tubeSides() const { return tubeSides_; }
    float tubeRadiusScale() const { return tubeRadiusScale_; }
    const std::vector<float>& tubeSegments() const { return tubeData_; }
    /// Vertices a full tube draw issues (segCount * sides * 6).
    size_t tubeVertexCount() const { return tubeSegCount_ * (size_t)tubeSides_ * 6; }

    void setMetallic(float m) { metallic_ = m; }
    float metallic() const { return metallic_; }

    void setRoughness(float r) { roughness_ = r; }
    float roughness() const { return roughness_; }

    void setEmissiveColor(float r, float g, float b) {
        emissiveColor_[0] = r; emissiveColor_[1] = g; emissiveColor_[2] = b;
    }
    const float* emissiveColor() const { return emissiveColor_; }

    void setDepthBias(float factor, float units) {
        depthBiasFactor_ = factor; depthBiasUnits_ = units;
    }
    float depthBiasFactor() const { return depthBiasFactor_; }
    float depthBiasUnits() const { return depthBiasUnits_; }

    void setNearClipDist(float d) { nearClipDist_ = d; }
    float nearClipDist() const { return nearClipDist_; }

    void setCastsShadow(bool b) { castsShadow_ = b; }
    bool castsShadow() const { return castsShadow_; }

    void setReceivesShadow(bool b) { receivesShadow_ = b; }
    bool receivesShadow() const { return receivesShadow_; }

    /// Atlas grid for the baseColor texture. (cols, rows) = (1, 1) means no
    /// atlas — the texture is sampled normally. With cols > 1 or rows > 1 the
    /// texture is treated as a cols x rows grid of cells, and each instance
    /// samples the cell selected by variantIndex packed into color.a by
    /// setInstancesFromPosQuatScale().
    void setAtlasGrid(int cols, int rows) {
        atlasCols_ = cols < 1 ? 1 : cols;
        atlasRows_ = rows < 1 ? 1 : rows;
        batchDirty_ = true;   // the batch bakes the atlas cell into its UVs
    }
    int atlasCols() const { return atlasCols_; }
    int atlasRows() const { return atlasRows_; }

    // --- Custom shader ---
    // Same surface as MeshNode (see custom_shader.h). The renderer compiles
    // the INSTANCED program variant — the vertex hook runs in mesh-local
    // space, before the per-instance transform, so a displacement applies
    // identically to every instance in its own frame. The depth-only shadow
    // pass keeps the undisplaced silhouette for instanced meshes.

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
    void setCustomShaderUniform(const std::string& name, int comps,
                                const float* vals) {
        if (customShader_) {
            customShader_->setUniform(name, comps, vals);
            bumpChangeGeneration();
        }
    }

    // --- Culling margin ---
    // Extra world-space padding added to the whole-node culling bounds —
    // same contract as MeshNode::setCullMargin (vertex displacement doesn't
    // grow bounds automatically).
    void setCullMargin(float m) { cullMargin_ = m < 0.0f ? 0.0f : m; }
    float cullMargin() const { return cullMargin_; }

    /// Compute the world-space AABB enclosing every instance's transformed
    /// local mesh bounds. Used by shadow frustum fitting. Returns false if
    /// there are no instances or the mesh is empty.
    bool computeWorldInstanceBounds(float outMin[3], float outMax[3]) const;

    bool hasVertexColors() const { return hasVertexColors_; }

    /// Whether the per-vertex color stream tints albedo. See
    /// MeshNode::setVertexColorTint — forcing it off keeps a color buffer
    /// as the wind-bend channel only, so instanced foliage sways without
    /// the bend gradient washing its material colour.
    void setVertexColorTint(bool b) { vertexColorTint_ = b ? 1 : 0; }
    bool vertexColorTintEnabled() const {
        return (vertexColorTint_ < 0) ? hasVertexColors_
                                      : (vertexColorTint_ != 0 && hasVertexColors_);
    }

private:
    // Rebake batchMesh_ from mesh_ + instanceData_ (static-batch path). Clears
    // batchDirty_ and bumps batchGeneration_.
    void rebuildStaticBatch() const;
    void instancesChanged();

    bromesh::MeshData mesh_;
    uint64_t geometryGeneration_ = nextResourceGeneration();
    bromath::AABB3 bounds_;

    // Cached BVH over mesh_, invalidated by setMesh. Covers the instance mesh
    // in its own local space; instance transforms are applied to the ray.
    mutable bromesh::MeshBVH bvh_;
    mutable bool bvhDirty_ = true;

    std::vector<float> instanceData_;
    size_t instanceCount_ = 0;

    // Static batching: mesh_ + instanceData_ merged into one draw. batchMesh_
    // is the baked geometry drawn (instead of mesh_) when renderingBatched().
    // Baked lazily on the render path, hence mutable.
    bool staticBatch_ = false;
    mutable bool batchDirty_ = true;
    mutable bromesh::MeshData batchMesh_;
    mutable uint64_t batchGeneration_ = 0;

    // Node-space union of instance-transformed mesh bounds, rebuilt lazily by
    // computeWorldInstanceBounds when the mesh or instances change. Cached
    // because frustum culling queries the bounds every frame and the rebuild
    // is O(instances).
    mutable bromath::AABB3 instanceBoundsCache_;
    mutable bool instanceBoundsDirty_ = true;

    // Scatter mode (see setScatterSegments): the packed segment records.
    bool scatterMode_ = false;
    std::vector<float> scatterData_;     // segCount*8 floats (2 RGBA32F texels)
    std::vector<float> scatterInstSeg_;  // instCount floats (leaf → segment idx)
    size_t scatterSegCount_ = 0;
    size_t scatterInstCount_ = 0;
    ScatterParams scatterParams_;
    bromath::AABB3 scatterBounds_;

    // Procedural branch-tube mode (see setTubeSegments): the packed segment
    // records.
    bool tubeMode_ = false;
    std::vector<float> tubeData_;   // segCount*8 floats (2 RGBA32F texels each)
    size_t tubeSegCount_ = 0;
    int    tubeSides_ = 6;
    float  tubeRadiusScale_ = 1.0f;
    bromath::AABB3 tubeBounds_;
    NodeTexture baseColorTex_;
    NodeTexture normalTex_;
    NodeTexture mrTex_;
    NodeTexture aoTex_;
    NodeTexture emissiveTex_;

    // Material
    bool hasVertexColors_ = false;
    // Albedo vertex-color tint: -1 auto, 0 forced off, 1 forced on.
    int  vertexColorTint_ = -1;
    float color_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive_ = 0.0f;
    float emissiveColor_[3] = {1.0f, 1.0f, 1.0f};
    float metallic_ = 0.0f;
    float roughness_ = 0.7f;
    bool  unlit_ = false;
    float alphaCutoff_ = 0.0f;
    bool  doubleSided_ = false;

    float depthBiasFactor_ = 0.0f;
    float depthBiasUnits_ = 0.0f;
    float nearClipDist_ = 0.0f;

    bool castsShadow_ = true;
    bool receivesShadow_ = true;

    int atlasCols_ = 1;
    int atlasRows_ = 1;

    // Custom shader chunks + user-uniform values (null = default pipeline).
    std::unique_ptr<CustomShaderState> customShader_;
    float cullMargin_ = 0.0f;
    ShadeMapProvider shadeMap_;
};

} // namespace bro::scene
