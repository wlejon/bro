// The probe grid's compute stages' set 0 (scene_probe_grid.cpp writes it
// for each dispatch) and push constants:
//   0  the block (ProbeGridGpu)
//   1  the ground's height (r32f storage: world heights, cascades stacked down it)
//   2  the capture's depth (the casters drawn straight down)
//   3  the splat's sums (PG_FIXED a unit, four words a voxel)
//   4  the canopy's medium (rgba16f storage: extinction (1/m), albedo)
//   5  the same, filtered
//   6  the sun's transmittance (r16f storage)
//   7  a plants node's records (scene_plants.glsl's)

#include "lighting/scene_probe_grid.glsl"

layout(set = 0, binding = 0) uniform ProbeGridBlock {
    ProbeGridGpu grid;
    vec4 capture;   // x the depth's half range (m about the eye), yzw 0
} block;

layout(set = 0, binding = 1, r32f) uniform image2D pgHeightImage;
layout(set = 0, binding = 2) uniform sampler2D pgCaptureDepth;
layout(std430, set = 0, binding = 3) buffer ProbeGridSums {
    uint sums[];
} pgSums;
layout(set = 0, binding = 4, rgba16f) uniform image3D pgMediumImage;
layout(set = 0, binding = 5) uniform sampler3D pgMedium;
layout(set = 0, binding = 6, r16f) uniform image3D pgSunImage;

// x the cascades this dispatch makes (a bit each), yzw the stage's own.
layout(push_constant) uniform ProbeGridPush {
    uvec4 job;
} push;

bool pgMaking(int c) { return (push.job.x & (1u << uint(c))) != 0u; }

#define PG block.grid
#define PG_HEIGHT_LOAD(texel) imageLoad(pgHeightImage, texel).r
#include "lighting/scene_probe_grid.glsl"
