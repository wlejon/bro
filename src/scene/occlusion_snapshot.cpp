#include "scene/occlusion_snapshot.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace bro::scene {

void OcclusionSnapshot::set(std::vector<Level> levels, int firstLevel, uint32_t frameWidth, uint32_t frameHeight,
                            const bromath::Mat4& proj, const bromath::Mat4& relView, const bromath::Vec3& eye,
                            bool reversedZ, uint64_t frame) {
    levels_ = std::move(levels);
    firstLevel_ = firstLevel;
    frameWidth_ = frameWidth;
    frameHeight_ = frameHeight;
    viewProj_ = bromath::mmul(proj, relView);
    eye_ = eye;
    reversedZ_ = reversedZ;
    frame_ = frame;
}

bool OcclusionSnapshot::occluded(const bromath::AABB3& box) const {
    if (levels_.empty() || frameWidth_ == 0 || frameHeight_ == 0) return false;
    // The box's screen rect and nearest depth, as scene_occlusion.glsl's
    // sceneHiZClipRect takes them.
    const bromath::Mat4& m = viewProj_;
    float loX = 1e30f, loY = 1e30f, hiX = -1e30f, hiY = -1e30f;
    float nearest = reversedZ_ ? 0.0f : 1.0f;
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {((i & 1) ? box.max.x : box.min.x) - eye_.x, ((i & 2) ? box.max.y : box.min.y) - eye_.y,
                            ((i & 4) ? box.max.z : box.min.z) - eye_.z};
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = m.at(r, 0) * p[0] + m.at(r, 1) * p[1] + m.at(r, 2) * p[2] + m.at(r, 3);
        if (c[3] <= 1e-6f) return false;
        const float z = c[2] / c[3];
        if (z < 0.0f || z > 1.0f) return false;
        const float x = (c[0] / c[3] * 0.5f + 0.5f) * static_cast<float>(frameWidth_);
        const float y = (c[1] / c[3] * 0.5f + 0.5f) * static_cast<float>(frameHeight_);
        loX = std::min(loX, x);
        loY = std::min(loY, y);
        hiX = std::max(hiX, x);
        hiY = std::max(hiY, y);
        nearest = reversedZ_ ? std::max(nearest, z) : std::min(nearest, z);
    }
    if (hiX < 0.0f || hiY < 0.0f || loX >= static_cast<float>(frameWidth_) || loY >= static_cast<float>(frameHeight_))
        return false;
    const auto clampPx = [](float v, uint32_t size) {
        return std::clamp(static_cast<int>(std::floor(v)), 0, static_cast<int>(size) - 1);
    };
    // Widened by a pixel, then in level 0's texels (half the frame's).
    const int x0 = std::max(clampPx(loX, frameWidth_) - 1, 0) >> 1;
    const int y0 = std::max(clampPx(loY, frameHeight_) - 1, 0) >> 1;
    const int x1 = std::min(clampPx(hiX, frameWidth_) + 1, static_cast<int>(frameWidth_) - 1) >> 1;
    const int y1 = std::min(clampPx(hiY, frameHeight_) + 1, static_cast<int>(frameHeight_) - 1) >> 1;
    // The level at which the rect spans at most two texels each way, no
    // finer than the levels kept.
    int level = firstLevel_;
    while ((x1 >> level) - (x0 >> level) > 1 || (y1 >> level) - (y0 >> level) > 1) ++level;
    const int last = firstLevel_ + static_cast<int>(levels_.size()) - 1;
    level = std::min(level, last);
    const Level& l = levels_[static_cast<size_t>(level - firstLevel_)];
    // (Past a level's last texel is still that texel: it folds in an odd
    // row or column.)
    const int ax = std::min(x0 >> level, static_cast<int>(l.width) - 1);
    const int ay = std::min(y0 >> level, static_cast<int>(l.height) - 1);
    const int bx = std::min(x1 >> level, static_cast<int>(l.width) - 1);
    const int by = std::min(y1 >> level, static_cast<int>(l.height) - 1);
    for (int y = ay; y <= by; ++y)
        for (int x = ax; x <= bx; ++x) {
            const float far = l.depth[static_cast<size_t>(y) * l.width + static_cast<size_t>(x)];
            if (reversedZ_ ? nearest >= far : nearest <= far) return false;
        }
    return true;
}

}  // namespace bro::scene
