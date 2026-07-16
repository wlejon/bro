#include "scene/skinned_mesh_node.h"
#include "scene/animation_player.h"
#include "util/log.h"

#include <cstring>

namespace bro::scene {

SkinnedMeshNode::SkinnedMeshNode(const std::string& name) : MeshNode(name) {}

SkinnedMeshNode::~SkinnedMeshNode() {
    releaseGL();
}

AnimationPlayer& SkinnedMeshNode::ensurePlayer() {
    if (!player_) player_ = std::make_unique<AnimationPlayer>(*this);
    return *player_;
}

void SkinnedMeshNode::onTick(float dtSec) {
    if (player_) player_->tick(dtSec);
}

bool SkinnedMeshNode::setSkin(const bromesh::SkinData& skin) {
    weights_.clear();
    joints_.clear();
    palette_.clear();
    boneCount_ = 0;
    skinVboDirty_ = true;
    paletteDirty_ = true;

    size_t bones = skin.boneCount;
    if (bones == 0 && !skin.inverseBindMatrices.empty())
        bones = skin.inverseBindMatrices.size() / 16;
    if (bones == 0 || bones > (size_t)kMaxBones) {
        LOG_WARN("SkinnedMeshNode::setSkin: bad bone count %zu (cap %d)",
                 bones, kMaxBones);
        return false;
    }
    if (skin.boneWeights.empty() ||
        skin.boneWeights.size() != skin.boneIndices.size() ||
        skin.boneWeights.size() % 4 != 0) {
        LOG_WARN("SkinnedMeshNode::setSkin: weight/index streams malformed "
                 "(%zu weights, %zu indices)",
                 skin.boneWeights.size(), skin.boneIndices.size());
        return false;
    }

    boneCount_ = (int)bones;
    weights_ = skin.boneWeights;
    joints_.resize(skin.boneIndices.size());
    for (size_t i = 0; i < skin.boneIndices.size(); ++i) {
        uint32_t j = skin.boneIndices[i];
        joints_[i] = (uint16_t)(j < bones ? j : 0);
    }

    // Identity palette = bind pose, so the node renders sensibly before the
    // first setSkinningMatrices call.
    palette_.assign((size_t)boneCount_ * 16, 0.0f);
    for (int b = 0; b < boneCount_; ++b) {
        float* m = &palette_[(size_t)b * 16];
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }
    return true;
}

int SkinnedMeshNode::setSkinningMatrices(const float* mats, size_t count) {
    if (!mats || boneCount_ == 0) return 0;
    size_t n = count < (size_t)boneCount_ ? count : (size_t)boneCount_;
    if (n == 0) return 0;
    std::memcpy(palette_.data(), mats, n * 16 * sizeof(float));
    paletteDirty_ = true;
    return (int)n;
}

void SkinnedMeshNode::uploadToGPU() {
    MeshNode::uploadToGPU();
    uploadSkinAttribs();
}

void SkinnedMeshNode::uploadSkinAttribs() {
    if (!vao_) return;
    skinVboDirty_ = false;
    if (!skinReady()) {
        // Mesh/skin mismatch — leave attributes 5/6 disabled; the renderer
        // draws this node through the static pipeline (skinReady() gates it).
        glBindVertexArray(vao_);
        glDisableVertexAttribArray(5);
        glDisableVertexAttribArray(6);
        glBindVertexArray(0);
        return;
    }

    // Interleave joints (4 x u16, 8 bytes) + weights (4 x float, 16 bytes):
    // 24-byte stride, weights 4-byte aligned at offset 8.
    size_t vertCount = weights_.size() / 4;
    std::vector<uint8_t> buf(vertCount * 24);
    for (size_t v = 0; v < vertCount; ++v) {
        uint8_t* dst = buf.data() + v * 24;
        std::memcpy(dst,     &joints_[v * 4],  4 * sizeof(uint16_t));
        std::memcpy(dst + 8, &weights_[v * 4], 4 * sizeof(float));
    }

    if (!skinVbo_) glGenBuffers(1, &skinVbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, skinVbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf.size(), buf.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(5);
    glVertexAttribIPointer(5, 4, GL_UNSIGNED_SHORT, 24, (void*)0);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, 24, (void*)8);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SkinnedMeshNode::prepareSkinnedDraw() {
    // Skin VBO changed after the mesh was already uploaded (setSkin on a
    // live node). If the mesh itself is dirty, drawRaw's uploadToGPU covers
    // this instead (vao_ may not even exist yet).
    if (skinVboDirty_ && vao_) uploadSkinAttribs();

    // Palette UBO: allocate at full cap once, sub-update the live range.
    if (!paletteUbo_) {
        glGenBuffers(1, &paletteUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, paletteUbo_);
        glBufferData(GL_UNIFORM_BUFFER, kMaxBones * 16 * sizeof(float),
                     nullptr, GL_DYNAMIC_DRAW);
        paletteDirty_ = true;
    }
    if (paletteDirty_ && !palette_.empty()) {
        glBindBuffer(GL_UNIFORM_BUFFER, paletteUbo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0,
                        (GLsizeiptr)(palette_.size() * sizeof(float)),
                        palette_.data());
        paletteDirty_ = false;
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, kPaletteBinding, paletteUbo_);
}

void SkinnedMeshNode::releaseGL() {
    if (skinVbo_)    { glDeleteBuffers(1, &skinVbo_);    skinVbo_ = 0; }
    if (paletteUbo_) { glDeleteBuffers(1, &paletteUbo_); paletteUbo_ = 0; }
    skinVboDirty_ = true;
    paletteDirty_ = true;
    MeshNode::releaseGL();
}

} // namespace bro::scene
