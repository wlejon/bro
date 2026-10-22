#pragma once
// The land's sun (docs/scene-world-architecture.md §5, "The land's sun"):
// how high a point must stand to see the sun, over the land about the eye,
// where the shadow pages cannot say. A low sun runs along the land and
// along a canopy's top, and the pages' depth offsets swallow a caster that
// lies along the light, so a hill's long shade and the shade a forest past
// the plants' cast range throws across a meadow come from here instead.
//
// LS_LEVELS square levels about the eye (ground/land_sun_layout.h), level
// l's texel LS_TEXEL0 * 2^l, each texel the world height above which the
// sun is seen there and how far off what shades it stands; land_sun.comp
// makes a level whole from the land's tiles, a world service job (§6) of
// its own, and every lit shader reads them (scene_lighting.glsl's
// sceneLandSun). This is the CPU half: where each level lies, which copy is
// shown, and which levels want making.
//
// A level has two copies. It is made into the copy not shown, and shown
// from then on: a lighting block written before the making still names the
// copy it was written with, and that copy is not touched until the next
// making, so a frame never reads a level half made or a window that moved
// under it. A level is centred on the eye, snapped to kSnapTexels texels of
// its own, and made again when
//   - it was never made (required: nothing else gives the land's shade);
//   - the eye moved it a snap (its square no longer centred);
//   - the sun turned more than kSunTurnCos since it was made;
//   - the land changed (its module, extent, rings or canopy: `generation`);
//   - the land's tiles changed since it was made (they refine as the eye
//     moves) and kRefreshFrames have passed.
// A want's error doubles every kRefreshFrames it waits, so one outranked
// frame after frame is still made.

#include <bromath/vec.h>

#include <array>
#include <cstdint>
#include <vector>

namespace bro::scene {

class LandSun {
public:
    static constexpr int kLevels = 6;          // LS_LEVELS
    static constexpr int kSide = 512;          // LS_SIDE
    static constexpr float kTexel0 = 2.0f;     // LS_TEXEL0
    static constexpr int kSnapTexels = 32;
    static constexpr float kSunTurnCos = 0.99999048f;   // a quarter of a degree
    static constexpr int kRefreshFrames = 30;

    /// Where a level lies and the copy (array layer) shown; layer -1 while it
    /// was never made.
    struct Level {
        double x0 = 0.0, z0 = 0.0;   // its corner (world x, z)
        float texel = 0.0f;
        int layer = -1;
    };

    /// A level that wants making this frame, and how badly: the world
    /// service's error (pixels) and whether it must be made now.
    struct Want {
        int level = 0;
        float error = 0.0f;
        bool required = false;
    };

    /// This frame's state: the eye, the sun (toward it), the land's
    /// generation and whether its tiles changed since the last call. Gathers
    /// wants().
    void update(const bromath::Vec3& eye, const bromath::Vec3& sun, uint64_t generation, bool tilesChanged);
    const std::vector<Want>& wants() const { return wants_; }

    /// Level `l` is made this frame: where (its corner and texel) and into
    /// which layer. It is shown from then on.
    Level make(int l);

    const Level& level(int l) const { return state_[static_cast<size_t>(l)].shown; }
    /// No land: nothing is shown, and every level is made again.
    void forget();

private:
    struct State {
        Level shown;
        double cx = 0.0, cz = 0.0;   // the centre wanted now
        bromath::Vec3 sun{0.0f, 1.0f, 0.0f};
        uint64_t generation = 0;
        bool changed = false;     // the tiles changed since it was made
        uint64_t madeFrame = 0;
        uint64_t wantedFrame = 0;   // since when it has wanted making (0: it does not)
    };
    std::array<State, kLevels> state_{};
    bromath::Vec3 sun_{0.0f, 1.0f, 0.0f};
    uint64_t generation_ = 0;
    uint64_t frame_ = 0;
    std::vector<Want> wants_;
};

}  // namespace bro::scene
