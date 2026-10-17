// A forest impostor's tree, for its card's vertex shaders
// (mesh_forest_impostor.vert, shadow_forest_impostor.vert): where the
// picture stands, how it is posed, its colour and which atlas block holds
// it. Needs scene_camera.glsl, scene_procedural.glsl, scene_vegetation.glsl,
// scene_forest.glsl and scene_forest_impostor.glsl.

struct ForestImpostorTree {
    mat4 model;          // specimen space to camera-relative world
    mat4 inv;            // and back
    vec3 center;         // the picture's centre at rest, camera-relative
    float radius;        // its half size, world
    vec2 shape;          // the upright cylinder round the centre holding it: radius, half height (world)
    vec3 sphere;         // its centre, specimen space
    float extent;        // its half size, specimen space
    vec3 xt;             // the trunk tip's deflection now (world)
    float height;        // the tree's height (world)
    vec3 xl;             // the crown's mean limb deflection now
    float rootY;         // camera-relative
    vec3 crown;          // the crown's centre, camera-relative
    float crownRadius;   // world
    vec3 leafTint;       // the per-tree part of the leaves' and bark's colour
    vec3 barkTint;
    float gust;          // the drag at the tree now
    float fade;          // the tier's fade code
    int specimen;
    ForestImpostorAtlas atlas;
};

bool forestImpostorTree(int entryIndex, mat4 base, out ForestImpostorTree t) {
    ForestEntry e = forestEntry(entryIndex);
    float treeSeed;
    t.model = base * forestInstance(e.instance, t.specimen, treeSeed);
    t.inv = inverse(t.model);
    float scale = vegScale(t.model);
    int sr = forestSpecimenRecord(t.specimen);
    vec4 counts = procedural.records[sr + 1];
    vec4 crown = procedural.records[sr + 2];
    vec4 sph = procedural.records[sr + 4];
    t.sphere = sph.xyz;
    t.extent = sph.w;
    t.radius = sph.w * scale;
    t.shape = procedural.records[sr + 5].xy * scale;
    t.center = (t.model * vec4(sph.xyz, 1.0)).xyz;
    t.fade = e.fade;

    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(int(floatBitsToUint(counts.z))) + 2, crown.xyz, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(t.model, v0, v3);
    VegLimb limb = vegLimb(t.model, v3);
    VegWind w = vegWind(plant);
    t.xt = vegLevel(plant.trunk, VEG_TRUNK_DAMPING, plant.seed, w, 0.0);
    float mw = clamp(counts.w, 0.0, 1.0);
    t.xl = limb.limb.sway > 0.0 && mw > 0.0 ? vegLevel(limb.limb, VEG_LIMB_DAMPING, limb.phase, w, 0.6) * vegBend(mw)
                                            : vec3(0.0);
    t.height = plant.height;
    t.rootY = plant.root.y;
    t.gust = w.p0;
    t.crown = (t.model * vec4(v1.xyz, 1.0)).xyz;
    t.crownRadius = max(v1.w * scale, 0.5);
    float tone = vegTreeTone(t.model, v0);
    t.leafTint = vegTreeLeafTint(tone);
    t.barkTint = vegTreeBarkTint(tone);

    vec4 h3 = procedural.header[3];
    t.atlas.views = int(h3.y + 0.5);
    t.atlas.texels = int(h3.z + 0.5);
    int blocksX = max(int(h3.w + 0.5), 1);
    t.atlas.block = ivec2(t.specimen % blocksX, t.specimen / blocksX) * t.atlas.views;
    return t.atlas.views > 0 && t.atlas.texels > 0 && t.radius > 0.0;
}

// How far the picture may move from rest: the trunk's and the limb's reach.
float forestImpostorReach(ForestImpostorTree t) { return 1.3 * length(t.xt) + length(t.xl); }

// The half sizes of the card along `right` (level) and `up` (the axes of a
// card facing direction f, unit, world) that hold the tree's cylinder seen
// orthographically from f, swayed by up to `reach`.
vec2 forestImpostorExtent(ForestImpostorTree t, vec3 f, float reach) {
    float c = abs(f.y), s = sqrt(max(1.0 - f.y * f.y, 0.0));
    return min(vec2(t.shape.x, t.shape.x * c + t.shape.y * s), vec2(t.radius)) + reach;
}
