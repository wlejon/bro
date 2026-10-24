// The field a far deck's share is cut into at the pixel, its clumps' relief,
// and their shadow's cut: one code for the clouds' layer
// (sky/scene_cloud_layer.glsl, clouds.frag) and for the land's shadow of it
// (common/scene_lighting.glsl's sceneCloudSun), so a clump's shadow on the
// ground is the same clump. The includer includes sky/clouds_layout.h first
// and defines cloudFieldTex: the engine's noise tile (CL_NOISE_SIDE^3,
// repeating, its mips) at `x` in tiles and mip `lod`.

vec4 cloudFieldTex(vec3 x, float lod);

// A turn (orthonormal) the cut's noises are read through in turn, so no
// two lattices line their clouds up in rows.
const mat3 kCloudCutTurn = mat3(0.36, 0.48, -0.8, -0.8, 0.6, 0.0, 0.48, 0.64, 0.6);

// A far field seen from above, cut to the pixel: `share` of a cell `cell`
// metres wide covered by clouds (a column's cover, the cache's or the
// globe's, bilinear between cells) as the share of a footprint `footprint`
// metres wide they cover, where the pixel is finer than the cell. The cell
// is cut by a field of the engine's noise (CLOUD_FIELD_OCTAVES octaves of
// its Perlin-Worley, the coarsest CLOUD_FIELD_TILE edges a tile, each the
// last's CLOUD_FIELD_STEP as large, turned in turn: clouds from a cell's
// size down to a cloud's edge, with broken, billowed rims), cloud where the
// field, as a score about normal, is over the share's quantile. Prefiltered
// by the footprint: each octave read at its mean over the footprint, with
// its spread within it (the noise's w, its square's mean) widening the
// score's step, so what is finer than the pixel is the share it covers and
// a footprint as wide as the cell reads the cell's share (and a finer
// octave fades out of the score as its clouds, about an eighth of its tile,
// come under CLOUD_FIELD_PIXELS pixels: clouds a pixel or two across read
// as speckle, so the finest shown are a few pixels, their rims still a
// pixel sharp; the score's spread is its octaves' that remain, so the
// share it covers is the same with or without them; the cut faded out
// from CLOUD_FIELD_FROM .. CLOUD_FIELD_TO pixels a cell), never grain. The
// score's spread from the tile's own mean and spread (its coarsest mip).
//
// The octaves' weights vary over the field (CLOUD_FIELD_VARY tiles of the
// coarsest a tile of a low read of the noise): where the finer octaves'
// fall off by CLOUD_FIELD_GAIN_LO the coarse clouds stand whole, big
// clumps, where by CLOUD_FIELD_GAIN_HI they break into small ones, so a
// field ranges from small to large across it. The score is spread back to
// one's wherever the weights, so the share it covers is the same.
const float CLOUD_FIELD_TILE = 40.0;
const float CLOUD_FIELD_STEP = 0.36;
const int CLOUD_FIELD_OCTAVES = 3;
const float CLOUD_FIELD_FROM = 4.0;
const float CLOUD_FIELD_TO = 10.0;
const float CLOUD_FIELD_PIXELS = 4.0;
const float CLOUD_FIELD_SOFT = 0.03;  // the score's least spread over a pixel (its edge a pixel or so)
const float CLOUD_FIELD_VARY = 3.0;
const float CLOUD_FIELD_GAIN_LO = 0.35;
const float CLOUD_FIELD_GAIN_HI = 1.0;

vec4 cloudFieldNoise(vec3 x, float footprint) {
    return cloudFieldTex(fract(x), log2(max(footprint * float(CL_NOISE_SIDE), 1.0)));
}

// How much the cut shows over a cell `cell` metres wide at `footprint`.
float cloudFieldFade(float cell, float footprint) {
    return smoothstep(CLOUD_FIELD_FROM, CLOUD_FIELD_TO, cell / max(footprint, 1.0));
}

// The field's score at drifted body point `pd` over `footprint` for clouds
// of edge `edge` (m): x about normal, y its spread within the footprint
// (a variance, of the score's).
vec2 cloudFieldScore(vec3 pd, float edge, float footprint) {
    vec4 n0 = cloudFieldTex(vec3(0.5), float(CL_NOISE_MIPS - 1));
    float M = n0.x, S = sqrt(max(n0.w - n0.x * n0.x, 1e-6));
    float T = CLOUD_FIELD_TILE * edge;
    // (The weights' low read: smooth over a coarse tile's eighth whatever
    // the footprint.)
    float V = CLOUD_FIELD_VARY * T;
    float u = smoothstep(-1.2, 1.2, (cloudFieldNoise(kCloudCutTurn * kCloudCutTurn * pd / V, max(footprint, T / 8.0) / V).x - M) / S);
    float gain = 0.7 * mix(CLOUD_FIELD_GAIN_LO, CLOUD_FIELD_GAIN_HI, u) / mix(CLOUD_FIELD_GAIN_LO, CLOUD_FIELD_GAIN_HI, 0.5);
    float z = 0.0, v = 0.0, aa = 0.0, a = 1.0;
    vec3 x = pd;
    for (int i = 0; i < CLOUD_FIELD_OCTAVES; ++i) {
        vec4 n = cloudFieldNoise(x / T, footprint / T);
        float ai = i == 0 ? a : a * smoothstep(0.5 * CLOUD_FIELD_PIXELS, CLOUD_FIELD_PIXELS, T / 8.0 / footprint);
        z += ai * (n.x - M);
        v += ai * ai * max(n.w - n.x * n.x, 0.0);
        aa += ai * ai;
        a *= gain;
        T *= CLOUD_FIELD_STEP;
        x = kCloudCutTurn * x;
    }
    float norm = S * sqrt(aa);
    return vec2(z / norm, v / (norm * norm));
}

// The score's threshold for a share: its quantile (logistic).
float cloudFieldThreshold(float share) {
    float s = clamp(share, 0.005, 0.995);
    return log((1.0 - s) / s) / 1.7;
}

// The share of the footprint over threshold `t` (the normal's step,
// logistic, widened by the score's spread within the footprint).
float cloudFieldCut(vec2 zv, float t) {
    return 1.0 / (1.0 + exp(-1.7 * (zv.x - t) / sqrt(zv.y + CLOUD_FIELD_SOFT)));
}

float cloudFieldShare(vec3 pd, float edge, float share, float cell, float footprint) {
    float fade = cloudFieldFade(cell, footprint);
    if (fade <= 0.0 || share <= 0.0 || share >= 1.0) return share;
    return mix(share, cloudFieldCut(cloudFieldScore(pd, edge, footprint), cloudFieldThreshold(share)), fade);
}
