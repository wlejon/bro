// The probe grid (docs/scene-world-architecture.md §7, "Design: the probe
// grid"): the numbers the CPU half (scene/world/probe_grid.h), the GPU half
// (scene/vulkan/lighting/scene_probe_grid.h) and its shaders
// (lighting/scene_probe_grid.glsl) agree on, for C++ and GLSL alike.
//
// Every cascade is the same square of PG_COLUMNS x PG_COLUMNS columns, its
// spacing its own. Its world covers the square and half a column past it on
// every side, so column i stands at the middle of its cells, and is stored
// wrapped as the probes are (a texel's place is its world texel mod the
// side: scene_probe_grid.glsl's pgHeightStored and pgVoxelStored), so a move
// remakes only the columns it brought in:
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
//
// The probes: l <= 1 SH of the radiance at each, a 3D image a colour
// channel (RGBA16F, PG_COLUMNS x PG_COLUMNS x PG_LAYERS * PG_MAX_CASCADES:
// column (i, j) of cascade c stored at its column mod PG_COLUMNS, layer k at
// depth c * PG_LAYERS + k), each texel (c0, cx, cy, cz): the radiance along
// d is c0 Y00 + PG_SH_Y1 (cx, cy, cz) . d. A probe fresh to its square is
// traced with PG_RAYS_FRESH rays and stored as traced; a refresh with
// PG_RAYS, blended into what it held by PG_REFRESH_BLEND. A trace writes a
// probe's SH in PG_TRACE_PARTS parts (its rays' 64s), the blend sums them.

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

#define PG_PROBES          (PG_COLUMNS * PG_COLUMNS * PG_LAYERS * PG_MAX_CASCADES)
#define PG_SH_Y00          0.282095
#define PG_SH_Y1           0.488603
#define PG_RAYS_FRESH      256
#define PG_RAYS            128
#define PG_REFRESH_BLEND   0.5
// A trace's workgroups a probe: a ray an invocation, 64 a workgroup, each
// part's SH summed by the blend.
#define PG_TRACE_PARTS     (PG_RAYS_FRESH / 64)

#endif
