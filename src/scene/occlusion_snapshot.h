#pragma once
// A view's occlusion on the CPU: the coarse levels of a frame's HiZ pyramid
// (the farthest depth drawn under each texel, scene_occlusion.glsl) read
// back with the view they were drawn from, so CPU work can skip what that
// frame's opaque depth hid: SurfaceLayers' tiles, far cards' cells. The
// test is the GPU's: a box whose nearest point lies behind the farthest
// depth over its screen rect (widened by a texel) was hidden. It is a few
// frames old (scene/vulkan/scene_occlusion_readback.h), so a user takes a
// box it calls hidden as a hint for what to spend on, never as a reason to
// draw nothing there: what the view turns to sees it hidden for those
// frames.
//
// The levels are the pyramid's from `firstLevel` up; a level is half the one
// below it, its last texel folding in an odd row or column, and level 0
// half the frame.

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene {

class OcclusionSnapshot {
public:
    struct Level {
        uint32_t width = 0, height = 0;
        std::vector<float> depth;   // row-major, the depth buffer's terms
    };

    /// Replace the pyramid: `levels` from the pyramid's level `firstLevel`,
    /// of a frame `frameWidth` x `frameHeight` pixels drawn from eye `eye`
    /// through `proj` (Vulkan clip space) x `relView` (the view without its
    /// translation), `reversedZ` the depth policy; `frame` the serial it was
    /// drawn at.
    void set(std::vector<Level> levels, int firstLevel, uint32_t frameWidth, uint32_t frameHeight,
             const bromath::Mat4& proj, const bromath::Mat4& relView, const bromath::Vec3& eye, bool reversedZ,
             uint64_t frame);
    void clear() { levels_.clear(); }
    bool valid() const { return !levels_.empty(); }
    /// The serial of the frame it was drawn at.
    uint64_t frame() const { return frame_; }

    /// Whether the frame's opaque depth hid the box (absolute world
    /// coordinates). Never for a box reaching behind the eye or past the
    /// depth range, nor wholly off the frame, nor without a pyramid.
    bool occluded(const bromath::AABB3& box) const;

private:
    std::vector<Level> levels_;
    int firstLevel_ = 0;
    uint32_t frameWidth_ = 0, frameHeight_ = 0;
    bromath::Mat4 viewProj_;
    bromath::Vec3 eye_{0.0f, 0.0f, 0.0f};
    bool reversedZ_ = true;
    uint64_t frame_ = 0;
};

}  // namespace bro::scene
