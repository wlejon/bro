// The probe grid's compute stages' set 0 (scene_probe_grid.cpp writes it
// for each dispatch) and push constants:
//   0  the block (ProbeGridGpu, and what the probes' traces light by)
//   1  the ground's height (r32f storage: world heights, cascades stacked down it)
//   2  the capture's depth (the casters drawn straight down)
//   3  the splat's sums (PG_FIXED a unit, four words a voxel)
//   4  the canopy's medium (rgba16f storage: extinction (1/m), albedo)
//   5  the same, filtered
//   6  the sun's transmittance (r16f storage)
//   7  a plants node's records (scene_plants.glsl's)
//   8  the sun's transmittance, filtered
//   9-11  the probes' SH, red, green, blue (rgba16f storage)
//   12-14 the same, filtered (repeating across: the columns wrap)
//   15 the probes a trace makes (one word each: pgProbe's packing)
//   16 the traces' SH, three vec4 a probe (the blend's input)
//   17 the atmosphere's transmittance table
//   18 the atmosphere's sky light table
//   19 the environment's prefiltered radiance

#include "common/scene_atmosphere_params.glsl"
#include "lighting/scene_probe_grid.glsl"

layout(set = 0, binding = 0) uniform ProbeGridBlock {
    ProbeGridGpu grid;
    vec4 capture;       // x the depth's half range (m about the eye), yzw 0
    vec4 sunLight;      // rgb the sun's illuminance (above the air when the atmosphere is on), w 1 with a sun
    vec4 sky;           // rgb the flat ambient, w the sky's light: 0 flat, 1 the environment, 2 the atmosphere
    vec4 environment;   // x intensity, y rotation (rad), z the prefiltered level read
    vec4 ground;        // rgb the ground's albedo, w the frame's seed
    AtmosphereGpu atm;
} block;

layout(set = 0, binding = 1, r32f) uniform image2D pgHeightImage;
layout(set = 0, binding = 2) uniform sampler2D pgCaptureDepth;
layout(std430, set = 0, binding = 3) buffer ProbeGridSums {
    uint sums[];
} pgSums;
layout(set = 0, binding = 4, rgba16f) uniform image3D pgMediumImage;
layout(set = 0, binding = 5) uniform sampler3D pgMedium;
layout(set = 0, binding = 6, r16f) uniform image3D pgSunImage;
layout(set = 0, binding = 8) uniform sampler3D pgSun;
layout(set = 0, binding = 9, rgba16f) uniform image3D pgShRImage;
layout(set = 0, binding = 10, rgba16f) uniform image3D pgShGImage;
layout(set = 0, binding = 11, rgba16f) uniform image3D pgShBImage;
layout(set = 0, binding = 12) uniform sampler3D pgShR;
layout(set = 0, binding = 13) uniform sampler3D pgShG;
layout(set = 0, binding = 14) uniform sampler3D pgShB;
layout(std430, set = 0, binding = 15) readonly buffer ProbeGridProbes {
    uint list[];
} pgProbes;
layout(std430, set = 0, binding = 16) buffer ProbeGridTraces {
    vec4 sh[];
} pgTraces;
layout(set = 0, binding = 17) uniform sampler2D pgAtmTransmittance;
layout(set = 0, binding = 18) uniform sampler3D pgAtmSkyLight;
layout(set = 0, binding = 19) uniform samplerCube pgPrefiltered;

// x the cascades this dispatch makes (a bit each), y the probes it traces,
// zw the stage's own.
layout(push_constant) uniform ProbeGridPush {
    uvec4 job;
} push;

bool pgMaking(int c) { return (push.job.x & (1u << uint(c))) != 0u; }

#define PG block.grid
#define PG_HEIGHT_LOAD(texel) imageLoad(pgHeightImage, texel).r
#include "lighting/scene_probe_grid.glsl"

// A probe a trace makes, packed: its cascade (2 bits), fresh (1), its
// column (5 + 5 bits, from the cascade's corner) and layer (3).
struct PgProbe {
    int cascade;
    bool fresh;
    ivec2 column;
    int layer;
};
PgProbe pgProbe(uint e) {
    PgProbe p;
    p.cascade = int(e & 3u);
    p.fresh = ((e >> 2) & 1u) != 0u;
    p.column = ivec2((e >> 3) & 31u, (e >> 8) & 31u);
    p.layer = int((e >> 13) & 7u);
    return p;
}
// Its texel in the SH images.
ivec3 pgProbeTexel(PgProbe p) {
    ivec2 at = (ivec2(PG.wrap[p.cascade].xy) + p.column) % PG_COLUMNS;
    return ivec3(at, p.cascade * PG_LAYERS + p.layer);
}
