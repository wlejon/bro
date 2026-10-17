// Herbs: which low plant grows where, the part every grass shader shares
// (the blades', the turf's and the herb plants' own: scene_herb.glsl).
// Included by scene_grass.glsl; needs scene_camera.glsl's noise.
//
// The grass node holds a table of herb species (broflora's herbRecord, 12
// vec4s each, GRASS_HERB_RECORD) and a grid of cells of its own (herbInfo.y
// m). Each cell holds at most one plant, and nothing about it is stored:
//   - each species weighs in at a point by its abundance, by how well the
//     light there suits it (the canopy overhead, the floor texture's r,
//     against its shade preference) and by its colonies (value noise of its
//     own period, its patchCover of the ground inside them), times how many
//     of its plants a cell holds (a fern a metre across stands in one cell
//     of several, a daisy in every one);
//   - the cell holds a plant with the weights' sum (to 1) times the floor's
//     herb density (its g), and which one by a hash among the weights.
// Far off, where the plants hand over to the grass's blades and the turf,
// herbMixAt gives the same weights' mean: how much of the ground the herbs
// cover from above and in what colours (their leaves', their blooms'), so
// the blades and the turf stand in for them with no step in colour.
//
// The record (broflora/herbs.h herbRecord):
//   0 habit, height, spread, leaves      6 bloomForm, bloomSize, bloomCount, petals
//   1 stems, leafShape, length, width    7 bloomColor, nod
//   2 tip, teeth, toothDepth, pinnae     8 centreColor, ribDiameter
//   3 lobes, stalk, rise, arch           9 shade, shadeRange, abundance, patch
//   4 leafColor, gloss                   10 patchCover, bloomShare, cover, 0
//   5 stemColor, stemDiameter            11 stem freq, stem sway, leaf freq, leaf sway

vec4 herbRec(int i, int k) { return grass.herbs[i * GRASS_HERB_RECORD + k]; }
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
    // closer than a cell, fewer where each spreads over several.
    float spacing = 1.6 * max(herbRec(i, 0).z, 0.02);
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

// What the herbs at `xz` look like from afar: how much of the ground their
// leaves and blooms cover from above, their leaves' and blooms' mean
// colours, the blooms' share of that cover and the plants' mean height.
struct HerbMix {
    float cover;
    vec3 leaf;
    vec3 bloom;
    float bloomShare;
    float height;        // the leaves' top
    float bloomHeight;   // the blooms'
};

// How high a species' leaves stand: a rosette's or a carpet's no higher
// than a leaf reaches at its rise (a daisy's lie low under its flowers), a
// stem's most of the way up it.
float herbLeafTop(int i) {
    vec4 h0 = herbRec(i, 0);
    if (int(h0.x + 0.5) == 1) return 0.7 * h0.y;
    return min(h0.y, herbRec(i, 1).z * cos(herbRec(i, 3).z));
}

HerbMix herbMixAt(vec2 xz, float canopy, float density) {
    HerbMix m;
    m.cover = 0.0;
    m.leaf = vec3(0.0);
    m.bloom = vec3(0.0);
    m.bloomShare = 0.0;
    m.height = 0.0;
    m.bloomHeight = 0.0;
    int n = herbCount();
    if (n <= 0 || density <= 0.0) return m;
    float cellArea = grass.herbInfo.y * grass.herbInfo.y;
    float sum = 0.0, area = 0.0, leafA = 0.0, bloomA = 0.0, hgt = 0.0, bhgt = 0.0;
    vec3 leaf = vec3(0.0), bloom = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        float w = herbWeight(i, xz, canopy);
        if (w <= 0.0) continue;
        vec4 h0 = herbRec(i, 0), h10 = herbRec(i, 10);
        // The ground one plant covers, in cells: its leaves' face from above
        // over its own patch, times that patch.
        float a = w * h10.z * 3.14159265 * h0.z * h0.z / cellArea;
        sum += w;
        area += a;
        leafA += a * (1.0 - h10.y);
        bloomA += a * h10.y;
        leaf += a * (1.0 - h10.y) * herbRec(i, 4).rgb;
        bloom += a * h10.y * herbRec(i, 7).rgb;
        hgt += w * herbLeafTop(i);
        bhgt += a * h10.y * h0.y;
    }
    if (sum <= 0.0) return m;
    // Each cell holds a plant with density x min(sum, 1); the weights' shares
    // pick which. Overlapping plants cover as a Poisson scatter would.
    float present = density * min(sum, 1.0) / sum;
    m.cover = 1.0 - exp(-present * area);
    m.leaf = leafA > 0.0 ? leaf / leafA : vec3(0.0);
    m.bloom = bloomA > 0.0 ? bloom / bloomA : vec3(0.0);
    m.bloomShare = area > 0.0 ? bloomA / area : 0.0;
    m.height = hgt / sum;
    m.bloomHeight = bloomA > 0.0 ? bhgt / bloomA : m.height;
    return m;
}

// herbMixAt's mean over the species' colonies (the node's table, by the
// canopy), for the turf: no noise, no loop over the species.
HerbMix herbMixMean(float canopy, float density) {
    float x = clamp(canopy, 0.0, 1.0) * float(GRASS_HERB_MEAN - 1);
    int i = min(int(x), GRASS_HERB_MEAN - 2);
    float f = x - float(i);
    vec4 a0 = mix(grass.herbMean[i * 2], grass.herbMean[i * 2 + 2], f);
    vec4 a1 = mix(grass.herbMean[i * 2 + 1], grass.herbMean[i * 2 + 3], f);
    HerbMix m;
    m.cover = 1.0 - exp(-density * a0.x);
    m.leaf = a0.yzw;
    m.bloomShare = a1.x;
    m.bloom = a1.yzw;
    m.height = 0.0;
    m.bloomHeight = 0.0;
    return m;
}

// How much of the herbs the plants draw at ring distance `d` (1 inside
// their range, falling through the hand-over band): the blades and the turf
// stand in for the rest.
float herbPlantsDrawn(float d) {
    if (grass.herbInfo.x < 0.5) return 0.0;
    return 1.0 - smoothstep(grass.herbInfo.z - grass.herbInfo.w, grass.herbInfo.z, d);
}
