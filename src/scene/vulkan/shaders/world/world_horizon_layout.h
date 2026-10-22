// The land's horizon about an eye (scene/vulkan/world/scene_world_horizon.h):
// the words of its buffer, for C++ and GLSL alike. Made from the land's tiles
// (ground/land_horizon.comp) and read by the cell pass (world_cells_cull.comp),
// which leaves out of the camera's view the cells the land hides.
//
// For each of WH_AZIMUTHS directions about the eye (bin a centred on the
// angle ((a + 0.5) / WH_AZIMUTHS - 0.5) * 2 pi, the angle atan(dx, dz) of a
// point dx, dz from the eye) and each of WH_STEPS distances (step k at
// WH_NEAREST * 2^(k / WH_PER_OCTAVE) metres), the steepest slope from the eye
// of the land along that direction from WH_NEAREST out to that distance:
// max((height - WH_MARGIN - eye.y) / distance), the land lowered by
// WH_MARGIN (what the drawn rings may sit below their tiles, and the eye's
// drift from the eye it was made at). A point whose slope from the eye is
// under that of every direction its box spans, at its nearest distance, is
// behind the land.

#ifndef BRO_SCENE_WORLD_HORIZON_LAYOUT_H
#define BRO_SCENE_WORLD_HORIZON_LAYOUT_H

#define WH_AZIMUTHS             1024
#define WH_STEPS                128   // a workgroup of the build a direction, a thread a step
#define WH_PER_OCTAVE           12.0  // steps a doubling of the distance
#define WH_NEAREST              16.0  // metres: the first step; nearer land hides nothing
#define WH_SAMPLES              4     // heights read a step (the build's)
#define WH_MARGIN               2.5   // metres
#define WH_SPAN_MOST            64    // directions: a box spanning more is not tested

#define WH_H_EYE                0     // xyz: the eye it was made about
#define WH_H_VALID              3     // 1 once made (0 in the cell pass's empty one)
#define WH_HEADER               4
#define WH_WORDS                (WH_HEADER + WH_AZIMUTHS * WH_STEPS)   // the slopes: step k's direction a at k * WH_AZIMUTHS + a

#endif
