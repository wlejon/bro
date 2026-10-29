#pragma once
// The world's BANDS: how far from the eye each way of drawing it reaches
// (docs/scene-api.js, setBands). Within `real` (m) the world is
// drawn as itself: every plant's elements, the grass's blades and the
// herbs; past it the land's canopy and cover stand for them. `painted` (m)
// is where the painted band ends and the sky map's painting takes over;
// past it (over half as far again) a land's turf is its surface lit as
// the sward, its blocks there drawn without the turf's patterns.
// One setting of the scene's (SceneGraph::setBands) that every node reads,
// so all of them hand over at the same place; close by default, the frame
// the 360 fps target is set for.

namespace bro::scene {

struct Bands {
    static constexpr float kRealDefault = 40.0f;
    float real = kRealDefault;
    float painted = 4000.0f;
};

}  // namespace bro::scene
