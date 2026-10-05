#include "scene/skinned_mesh_node.h"
#include "scene/animation_player.h"
#include "util/log.h"

#include <cstring>

namespace bro::scene {

SkinnedMeshNode::SkinnedMeshNode(const std::string& name) : MeshNode(name) {}

SkinnedMeshNode::~SkinnedMeshNode() = default;

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
    skinGeneration_ = nextResourceGeneration();

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

bromath::AABB3 SkinnedMeshNode::posedLocalBounds() const {
    const bromath::AABB3& bind = localBounds();
    if (boneCount_ <= 0 || palette_.size() < (size_t)boneCount_ * 16)
        return bind;
    bromath::AABB3 out = bromath::aempty3();
    for (int b = 0; b < boneCount_; ++b) {
        bromath::Mat4 m;
        std::memcpy(m.data, &palette_[(size_t)b * 16], sizeof(float) * 16);
        out = bromath::amerge(out, bromath::atransform(bind, m));
    }
    return out;
}

int SkinnedMeshNode::setSkinningMatrices(const float* mats, size_t count) {
    if (!mats || boneCount_ == 0) return 0;
    size_t n = count < (size_t)boneCount_ ? count : (size_t)boneCount_;
    if (n == 0) return 0;
    std::memcpy(palette_.data(), mats, n * 16 * sizeof(float));
    return (int)n;
}

} // namespace bro::scene
