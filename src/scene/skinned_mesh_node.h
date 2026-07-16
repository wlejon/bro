#pragma once

#include "scene/mesh_node.h"

#include <cstdint>
#include <vector>

namespace bro::scene {

/// GPU-skinned mesh node. Extends MeshNode with per-vertex joint/weight
/// attribute streams (from bromesh::SkinData) and a bone matrix palette that
/// is applied in the vertex shader — positions, normals, and tangents are all
/// transformed by the weighted palette before the node's own model matrix, so
/// position/rotation/scale still compose on top exactly like MeshNode.
///
/// The palette holds FINAL skinning matrices, i.e. world(bone) * inverseBind
/// per bone — exactly what bromesh::computeSkinningMatrices produces (16
/// floats per bone, column-major). Driving an animation therefore never
/// re-uploads the mesh: evaluate → blend → computeSkinningMatrices →
/// setSkinningMatrices, and the vertex buffer stays put.
///
/// Keeps Type::Mesh (see MeshNode::asSkinnedMesh) so all Mesh-typed walks —
/// shadow caster gather, bounds, raycast (against the bind pose), material
/// paths — work unchanged. The renderer routes nodes with a ready skin to the
/// SKINNED shader variants; a node whose skin doesn't match the mesh (e.g.
/// after updateMesh with different topology) falls back to a static draw.
class SkinnedMeshNode : public MeshNode {
public:
    /// Bone cap = palette UBO size. The palette lives in a std140 uniform
    /// block of 256 mat4s = 16 KB, which is exactly GL 3.3 core's guaranteed
    /// minimum GL_MAX_UNIFORM_BLOCK_SIZE — so the cap never depends on the
    /// driver. A float-texture palette could lift the cap but costs 4 texel
    /// fetches per matrix per vertex and burns a sampler unit in the depth-
    /// only shadow pass; 256 comfortably covers character rigs (humanoids
    /// run ~60-180 bones), so the UBO is the better trade.
    static constexpr int kMaxBones = 256;

    /// Uniform-buffer binding point the palette binds to. Shared by the
    /// skinned mesh program and the skinned shadow program (both declare
    /// block "BonePalette" bound here).
    static constexpr int kPaletteBinding = 0;

    explicit SkinnedMeshNode(const std::string& name = "");
    ~SkinnedMeshNode() override;

    SkinnedMeshNode* asSkinnedMesh() override { return this; }

    // --- Skin data ---

    /// Set per-vertex joints/weights (copies boneWeights + boneIndices from
    /// the SkinData; inverseBindMatrices are NOT consumed here — they're
    /// already folded into the skinning matrices the palette receives).
    /// Resets the palette to identity (bind pose). Returns false (and clears
    /// the skin) when the data is malformed: no bones, missing/mismatched
    /// weight and index streams, or boneCount > kMaxBones.
    bool setSkin(const bromesh::SkinData& skin);

    /// Number of bones in the palette (0 when no skin is set).
    int boneCount() const { return boneCount_; }

    /// True when the node has a skin whose vertex count matches the current
    /// mesh — the condition for rendering through the skinned pipeline.
    /// Re-checked against the mesh on every call so updateMesh() with a
    /// mismatched mesh degrades to a static (bind-buffer) draw, not UB.
    bool skinReady() const {
        return boneCount_ > 0 && !weights_.empty() &&
               weights_.size() / 4 == mesh().vertexCount();
    }

    // --- Palette ---

    /// Upload skinning matrices: `mats` is count * 16 floats, column-major
    /// 4x4, laid out exactly like bromesh::computeSkinningMatrices output
    /// (world(bone) * inverseBind(bone)). Copies min(count, boneCount) — the
    /// GPU upload happens on the next render. Returns the number of matrices
    /// staged. Callable every frame from the JS thread; cheap memcpy.
    int setSkinningMatrices(const float* mats, size_t count);

    // --- GL-thread hooks (renderer only) ---

    /// Called by the renderer right before a skinned draw: flushes a dirty
    /// skin attribute VBO into the VAO (when the mesh itself didn't change),
    /// flushes a dirty palette into the UBO, and binds the UBO to
    /// kPaletteBinding. Must run on the GL thread with no VAO bound.
    void prepareSkinnedDraw();

    void releaseGL() override;

protected:
    /// MeshNode upload + joint/weight streams appended to the same VAO as
    /// attributes 5 (uvec4 joints, u16) and 6 (vec4 weights, float).
    void uploadToGPU() override;

private:
    void uploadSkinAttribs();

    // CPU-side skin streams (4 per vertex each). Joints stored as u16 —
    // kMaxBones is 256 so the narrowing is always lossless.
    std::vector<float>    weights_;
    std::vector<uint16_t> joints_;
    int  boneCount_ = 0;
    bool skinVboDirty_ = false;

    // Palette staging (boneCount_ * 16 floats) + GPU objects.
    std::vector<float> palette_;
    bool paletteDirty_ = false;

    GLuint skinVbo_ = 0;
    GLuint paletteUbo_ = 0;
};

} // namespace bro::scene
