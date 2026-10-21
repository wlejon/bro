// The probe grid (docs/scene-world-architecture.md §7, "Design: the probe
// grid"): the numbers the CPU half (scene/world/probe_grid.h), the GPU half
// (scene/vulkan/lighting/scene_probe_grid.h) and its shaders
// (lighting/scene_probe_grid.glsl) agree on, for C++ and GLSL alike.
//
// Every cascade is the same square of PG_COLUMNS x PG_COLUMNS columns, its
// spacing its own. Its world, made for it when it moves (and never
// wrapped), covers the square and half a column past it on every side, so
// column i stands at the middle of its cells:
//   - the ground's height, PG_HEIGHT x PG_HEIGHT texels (four a column),
//     the cascades stacked down one 2D image (cascade c's rows
//     c * PG_HEIGHT ..);
//   - the canopy, PG_VOXELS x PG_VOXELS x PG_LAYERS_Y voxels (two a column
//     across, PG_LAYERS_Y up from the ground to the top of the probes'
//     layers), the cascades stacked along the 3D images' depth: the medium
//     (extinction, albedo) and the sun's light through it (transmittance).
// The splat sums a cascade's canopy in fixed point (PG_FIXED a unit), four
// words a voxel: the extinction, and the extinction times the albedo's red,
// green and blue.

#ifndef BRO_SCENE_PROBE_GRID_LAYOUT_H
#define BRO_SCENE_PROBE_GRID_LAYOUT_H

#define PG_MAX_CASCADES 4
#define PG_COLUMNS      32
#define PG_LAYERS       8
#define PG_HEIGHT       128
#define PG_VOXELS       64
#define PG_LAYERS_Y     32
#define PG_FIXED        1048576.0

// A hole in the ground's height (nothing captured there): any height under
// this is one.
#define PG_HOLE         -1.0e30
#define PG_HOLE_TEST    -1.0e29

// A splatted crown's samples: PG_SAMPLES_PER_VOXEL for each voxel its
// volume fills, between PG_MIN_SAMPLES and PG_MAX_SAMPLES.
#define PG_SAMPLES_PER_VOXEL 32.0
#define PG_MIN_SAMPLES       64
#define PG_MAX_SAMPLES       16384

#endif
