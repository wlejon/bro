// Herbs: which low plant grows where, the part every grass shader shares
// (the herb plants' own, scene_herb.glsl, and the turf's). Included by
// scene_grass.glsl; needs scene_camera.glsl's noise.
//
// The grass node holds a table of herb species (broflora's herbRecord, 13
// vec4s each, GRASS_HERB_RECORD) and a grid of cells of its own (herbInfo.y
// m). Each cell holds at most one plant, and nothing about it is stored:
//   - each species weighs in at a point by its abundance, by how well the
//     light there suits it (the canopy overhead, the floor texture's r,
//     against its shade preference) and by its colonies (value noise of its
//     own period, its patchCover of the ground inside them), times how many
//     of its plants a cell holds (a daisy in every cell, a foxglove in one
//     of several: its spacing);
//   - the cell holds a plant with the weights' sum (to 1) times the floor's
//     herb density (its g), and which one by a hash among the weights.
// Past the far plants, the turf takes the herbs' mean over their colonies
// (herbMixMean, the node's table by canopy): how much of the ground they
// cover seen from above and from the side, in what colours.
//
// The record (broflora/herbs.h herbRecord):
//   0 habit, height, spread, leaves      7 bloomColor, nod
//   1 stems, leafShape, length, width    8 centreColor, ribDiameter
//   2 tip, teeth, toothDepth, pinnae     9 shade, shadeRange, abundance, patch
//   3 lobes, stalk, rise, arch           10 patchCover, bloomShare, cover, hairs
//   4 leafColor, gloss                   11 stem freq, stem sway, leaf freq, leaf sway
//   5 stemColor, stemDiameter            12 spacing, side cover, its blooms' share, leaf top
//   6 bloomForm, bloomSize, bloomCount, petals

vec4 herbRec(int i, int k) { return grass.herbs[i * GRASS_HERB_RECORD + k]; }

// A hairy stem's card reaches this many of the stem's diameters past each
// side, times its hairs, for the hairs to stand in.
const float HERB_STEM_HAIR_ROOM = 0.6;
int herbCount() { return min(int(grass.herbInfo.x + 0.5), GRASS_HERB_MAX); }

// Species `i`'s weight at absolute `xz` under canopy `canopy`.
float herbWeight(int i, vec2 xz, float canopy) {
    vec4 h9 = herbRec(i, 9);
    if (h9.z <= 0.0) return 0.0;
    float dl = (canopy - h9.x) / max(h9.y, 0.05);
    float light = exp(-dl * dl);
    if (light < 0.03) return 0.0;
    // Its colonies: two octaves of value noise at its own period, offset
    // species by species; the threshold leaves about patchCover inside.
    vec2 p = xz / max(h9.w, 0.5) + vec2(float(i) * 37.13 + 11.0, float(i) * -19.71 + 3.0);
    float n = 0.5 + 0.33 * sceneWindNoise(p) + 0.17 * sceneWindNoise(p * 2.7 + 5.1);
    float t = mix(0.7, 0.3, clamp(herbRec(i, 10).x, 0.0, 1.0));
    float colony = smoothstep(t - 0.07, t + 0.07, n);
    // How many of its plants a cell holds: one a cell where they stand
    // closer than a cell, fewer where they stand further apart.
    float s = herbRec(i, 12).x;
    float spacing = s > 0.0 ? s : 1.6 * max(herbRec(i, 0).z, 0.02);
    float occupy = min(1.0, pow(grass.herbInfo.y / spacing, 2.0));
    return h9.z * light * colony * occupy;
}

// The plant in a cell at `xz` (canopy `canopy`, herb density `density`),
// from two uniform draws: its species, or -1 for none.
int herbPick(vec2 xz, float canopy, float density, float u0, float u1) {
    int n = herbCount();
    // (The cell holds a plant with at most the density's chance: most cells
    // of a thin floor are settled before any species is weighed.)
    if (n <= 0 || u0 >= density) return -1;
    float w[GRASS_HERB_MAX];
    float sum = 0.0;
    for (int i = 0; i < GRASS_HERB_MAX; ++i) {
        w[i] = i < n ? herbWeight(i, xz, canopy) : 0.0;
        sum += w[i];
    }
    if (sum <= 0.0 || u0 >= density * min(sum, 1.0)) return -1;
    float pick = u1 * sum;
    for (int i = 0; i < n; ++i) {
        pick -= w[i];
        if (pick < 0.0) return i;
    }
    return n - 1;
}

// What the herbs look like from afar over a stretch of ground: the share of
// it they cover seen from the eye, their leaves' and blooms' mean colours
// and the blooms' share of what is seen.
struct HerbMix {
    float cover;
    vec3 leaf;
    vec3 bloom;
    float bloomShare;
};

// herbMixMean's view: the herbs' mean over their colonies (the node's
// table, by the canopy), seen at an elevation whose tangent is `tanView`
// (a grazing view sees the plants' sides over the ground between them, so
// a meadow seen across reads as its flowers and leaves, from above as much
// ground as they leave bare). No noise, no loop over the species.
HerbMix herbMixMean(float canopy, float density, float tanView) {
    float x = clamp(canopy, 0.0, 1.0) * float(GRASS_HERB_MEAN - 1);
    int i = min(int(x), GRASS_HERB_MEAN - 2);
    float f = x - float(i);
    vec4 a0 = mix(grass.herbMean[i * 3], grass.herbMean[i * 3 + 3], f);
    vec4 a1 = mix(grass.herbMean[i * 3 + 1], grass.herbMean[i * 3 + 4], f);
    vec4 a2 = mix(grass.herbMean[i * 3 + 2], grass.herbMean[i * 3 + 5], f);
    float side = a2.x / max(tanView, 0.02);
    HerbMix m;
    m.cover = a2.z * (1.0 - exp(-density * (a0.x + side)));
    m.leaf = a0.yzw;
    m.bloom = a1.yzw;
    m.bloomShare = (a0.x * a1.x + side * a2.y) / max(a0.x + side, 1e-5);
    return m;
}

// The share of the far plants drawn at ring distance `d` seen at an
// elevation whose tangent is `tanView` (the eye's lift over the distance
// across the ground, as the rings measure it): seen from well above, a far plant is
// a dot the turf's cover from above stands for as well, so they are left to
// it (a view down over the land from a height draws none).
float herbFarSteep(float d, float tanView) {
    float far = smoothstep(grass.herbShape.w, grass.herbShape.w * 1.4, d);
    return 1.0 - far * smoothstep(0.25, 0.5, tanView);
}

// How much of the herbs the plants draw at ring distance `d` (1 inside the
// far plants' range, falling through their hand-over band and as the view
// steepens past the whole plants): the turf stands in for the rest.
float herbPlantsDrawn(float d, float tanView) {
    if (grass.herbInfo.x < 0.5) return 0.0;
    return (1.0 - smoothstep(grass.herbInfo.z - grass.herbInfo.w, grass.herbInfo.z, d)) * herbFarSteep(d, tanView);
}
