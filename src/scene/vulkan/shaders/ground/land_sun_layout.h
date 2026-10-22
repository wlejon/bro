// The land's sun (scene/world/land_sun.h): about the eye, LS_LEVELS square
// levels of LS_SIDE texels, level l's texel LS_TEXEL0 * 2^l metres, each
// texel the world height above which the sun is seen there (the land and
// the canopy past the plants' cast range shading it) and how far off what
// shades it stands. For C++ and GLSL alike.
//
//   land_sun.comp's set 0: binding 0 the land's LandDraw words, 1 its
//            heights, 2 its surface, 3 its page table, 4 its canopy (the
//            land's tiles, ground/scene_land.glsl), 5 the levels (rg32f
//            storage, an array layer a level's copy); push constants LS_P_*
//            (floats)
//
//   the lighting set's binding 23: the levels, sampled (LandSunGpu in the
//            lighting block says where each level lies and which copy is
//            shown)

#ifndef BRO_SCENE_LAND_SUN_LAYOUT_H
#define BRO_SCENE_LAND_SUN_LAYOUT_H

#define LS_LEVELS           6
#define LS_SIDE             512
#define LS_TEXEL0           2.0      // level 0's texel (m): its square reaches 512 m either way of its centre
#define LS_LAYERS           (2 * LS_LEVELS)   // two copies a level: the shown one and the one being made
#define LS_GROUP            8
#define LS_STEPS            64       // the most steps a texel's ray takes toward the sun
#define LS_GROWTH           0.12     // a step is this share of the way come (at least a texel)

// --- land_sun.comp's push constants (floats) ---
#define LS_P_ORIGIN         0     // x, z of the level's corner (world), its texel (m), the array layer it writes
#define LS_P_SUN            4     // xyz toward the sun, w the highest anything shading it stands (world m)
#define LS_P_WORDS          8

#endif
