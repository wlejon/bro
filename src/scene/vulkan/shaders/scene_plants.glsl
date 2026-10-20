// Plants (scene/plants_node.h; drawn by scene_mesh_drawer_plants.cpp):
// every plant is built where it stands from its species' record and its
// seed, nothing stored per leaf. A plant is a crown of LOBES (clumps of
// foliage round its limbs) and a trunk with a limb to each lobe; its
// foliage is ELEMENTS, opaque leaf-shaped polygons (no alpha test: the
// depth test rejects what is hidden before it shades).
//
// One representation at every distance: a plant drawn with N elements
// draws the first N of one fixed sequence. They go round its lobes (element
// k is in lobe k mod lobes), so every prefix covers the whole crown, and
// each grows as N falls (by sqrt(full / N)) so the crown stays covered: up
// close a tree is thousands of leaves, far off a few dozen broad daubs of
// its colours. The last element grows in with N's fraction, so nothing pops.
//
// Set 3 binding 1 (the procedural slot), written each frame by the drawer:
//   header[0]  plants, species, elements a chunk, the first species record
//   header[1]  the first plant record, the first chunk record, the first
//              disc record, 0
//   header[2]  0, 0, 0, 0
//   header[3]  0, 0, 0, 0
//   records[]  species (PLANT_SPECIES_VEC4 each), plants (2 each: x, y, z,
//              yaw; scale, species, seed (an integer under 2^24), tint),
//              chunks (1 each: plant, first element, the plant's element
//              count N (a wood chunk: its tubes), 1 for a shadow tile's
//              caster else 0); a chunk of crown discs (shadow_plants.vert):
//              its first disc record, how many, 0, 2
//   discs      (1 each: plant, 0, 0, 0)

layout(std430, set = 3, binding = 1) readonly buffer PlantRecords {
    vec4 header[4];
    vec4 records[];
} plants;

#define PLANT_SPECIES_VEC4 8
// The leaf polygon: base, two pairs up its sides and the tip, as 4 triangles.
#define PLANT_LEAF_VERTS 12
// A limb or the trunk: a tapered tube of 6 sides, one span (6 quads).
#define PLANT_TUBE_SIDES 6
#define PLANT_TUBE_VERTS (PLANT_TUBE_SIDES * 6)
#define PLANT_MAX_LOBES 48

uint plantHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float plantRand(uint x) { return float(plantHash(x) >> 8) * (1.0 / 16777216.0); }
float plantRand(uint a, uint b) { return plantRand(a * 0x9e3779b9u + plantHash(b)); }

struct PlantSpecies {
    float height;      // m, at scale 1
    float base;        // where the crown starts, a fraction of the height
    float radius;      // the crown's radius (m)
    float shape;       // 0 a round crown .. 1 a cone
    float lobes;       // clumps in the crown
    float lobeSize;    // a lobe's radius, a fraction of the crown's
    float leafLen;     // a leaf's length (m) at full detail
    float leafWidth;   // its width over its length
    float full;        // elements at full detail
    float trunk;       // the trunk's radius at the foot (m)
    float spread;      // how far the lobes sit out from the trunk, 0..1
    float droop;       // how far leaves hang, 0..1
    vec3 light;        // the foliage's sunlit colour (linear)
    float hueVar;      // plant to plant and lobe to lobe colour variety
    vec3 dark;         // its colour deep in the crown
    float glow;        // how much the sun shows through it from behind
    vec3 bark;         // the trunk's colour
    float density;     // how dense the crown is to the sun's light (1/m)
    vec3 glowColor;    // the colour of the light through it
    float needle;      // 0 broad leaves .. 1 needle sprays (narrow, dark, stiff)
};

PlantSpecies plantSpecies(int i) {
    int r = int(plants.header[0].w) + i * PLANT_SPECIES_VEC4;
    vec4 a = plants.records[r], b = plants.records[r + 1], c = plants.records[r + 2];
    vec4 d = plants.records[r + 3], e = plants.records[r + 4], f = plants.records[r + 5];
    vec4 g = plants.records[r + 6];
    PlantSpecies s;
    s.height = a.x; s.base = a.y; s.radius = a.z; s.shape = a.w;
    s.lobes = b.x; s.lobeSize = b.y; s.leafLen = b.z; s.leafWidth = b.w;
    s.full = c.x; s.trunk = c.y; s.spread = c.z; s.droop = c.w;
    s.light = d.xyz; s.hueVar = d.w;
    s.dark = e.xyz; s.glow = e.w;
    s.bark = f.xyz; s.density = f.w;
    s.glowColor = g.xyz; s.needle = g.w;
    return s;
}

struct Plant {
    vec3 pos;      // absolute world
    float yaw;
    float scale;
    int species;
    uint seed;
    float tint;    // -1..1: a shift of its colours toward the dark or the light
};

Plant plantAt(int i) {
    int r = int(plants.header[1].x) + i * 2;
    vec4 a = plants.records[r], b = plants.records[r + 1];
    Plant p;
    p.pos = a.xyz;
    p.yaw = a.w;
    p.scale = b.x;
    p.species = int(b.y);
    p.seed = plantHash(uint(b.z) + 0x51ed27u);
    p.tint = b.w;
    return p;
}

// The crown's envelope in the plant's frame (y up, metres): an ellipsoid
// about `center` with `radii`, narrowed toward its top by `shape` (a cone at 1).
struct PlantCrown {
    vec3 center;
    vec3 radii;
    float shape;
};

PlantCrown plantCrown(PlantSpecies s, float scale) {
    PlantCrown c;
    float h = s.height * scale;
    float b = h * s.base;
    c.center = vec3(0.0, 0.5 * (b + h), 0.0);
    c.radii = vec3(s.radius * scale, 0.5 * (h - b), s.radius * scale);
    c.shape = s.shape;
    return c;
}

// The crown's horizontal radius at height v (-1 its foot .. 1 its top), over its full radius.
float plantCrownWidth(PlantCrown c, float v) {
    float round_ = sqrt(max(1.0 - v * v, 0.0));
    float cone = clamp(0.5 * (1.0 - v), 0.0, 1.0) * 1.1;
    return mix(round_, cone, c.shape);
}

// Lobe `j` of a plant: its centre (plant frame) and radius. The lobes lie
// over the envelope on a golden-angle spiral from the top down, jittered by
// the seed, each pulled in by its own radius so the crown's outline is
// theirs: lumpy, with sky through the gaps between them.
vec4 plantLobe(PlantSpecies s, PlantCrown c, uint seed, int j) {
    float n = max(s.lobes, 1.0);
    float u = (float(j) + 0.5 + 0.6 * (plantRand(seed, uint(j) * 7u + 1u) - 0.5)) / n;
    // Lobes thin toward the crown's foot, where its lowest limbs reach out.
    float v = 1.0 - 2.0 * pow(u, 0.85);
    float ang = float(j) * 2.39996323 + plantRand(seed, 3u) * 6.2831853 + 0.5 * plantRand(seed, uint(j) * 7u + 2u);
    float w = plantCrownWidth(c, v);
    float lr = s.lobeSize * c.radii.x * (0.7 + 0.6 * plantRand(seed, uint(j) * 7u + 3u)) *
               mix(1.0, 0.55 + 0.45 * w, c.shape);
    float out_ = max(w * c.radii.x - 0.8 * lr, 0.0) * mix(0.55, 1.0, s.spread);
    // A round crown's clumps fill it, some deep inside, so it is a mass and
    // not a shell; a cone's are its branches' tips, out at its skin.
    out_ *= mix(0.3 + 0.7 * sqrt(plantRand(seed, uint(j) * 7u + 5u)), 1.0, c.shape);
    vec3 at = c.center + vec3(cos(ang) * out_, v * max(c.radii.y - 0.6 * lr, 0.0), sin(ang) * out_);
    return vec4(at, lr);
}

// The plant's frame to the world: turned by its yaw about its foot.
vec3 plantToWorld(Plant p, vec3 local) {
    float cs = cos(p.yaw), sn = sin(p.yaw);
    return p.pos + vec3(cs * local.x - sn * local.z, local.y, sn * local.x + cs * local.z);
}
vec3 plantDirToWorld(Plant p, vec3 d) {
    float cs = cos(p.yaw), sn = sin(p.yaw);
    return vec3(cs * d.x - sn * d.z, d.y, sn * d.x + cs * d.z);
}

// The wind's push on a point `up` metres up a plant `height` tall: the
// trunk bends as a cantilever (the square of the height up it), each lobe
// rides it with a lag of its own. World metres, horizontal.
vec3 plantSway(Plant p, float up, float height, float lag) {
    vec4 w = sceneWind(p.pos - camera.eyeWorld.xyz);
    float t = camera.windParams.x;
    float f = clamp(up / max(height, 0.1), 0.0, 1.2);
    float bend = f * f * (0.012 + 0.03 * w.w) * height;
    float sway = 0.6 + 0.4 * sin(t * 1.3 + plantRand(p.seed, 11u) * 6.2831853 + lag);
    return vec3(w.x, 0.0, w.z) * (bend * sway / max(length(w.xz), 0.5));
}

// A random unit vector from two uniforms.
vec3 plantSphere(float a, float b) {
    float z = 2.0 * a - 1.0;
    float r = sqrt(max(1.0 - z * z, 0.0));
    float t = 6.2831853 * b;
    return vec3(r * cos(t), z, r * sin(t));
}

// The leaf polygon's point `c` (0..PLANT_LEAF_POINTS-1: base, left 1,
// right 1, left 2, right 2, tip) as (across, along) in [-0.5, 0.5] x [0, 1].
// Drawn indexed, PLANT_LEAF_VERTS indices an element (the drawer's
// leafIndices: 0 2 1, 1 2 4, 1 4 3, 3 4 5).
#define PLANT_LEAF_POINTS 6
vec2 plantLeafCorner(int c, float needle) {
    const vec2 pts[6] = vec2[6](vec2(0.0, 0.0), vec2(-0.48, 0.32), vec2(0.48, 0.32),
                                vec2(-0.36, 0.72), vec2(0.36, 0.72), vec2(0.0, 1.0));
    vec2 q = pts[c];
    // Needle sprays: narrower at the base, square at the tip (a fan of shoots).
    q.x *= mix(1.0, q.y < 0.5 ? 0.6 : 1.1, needle);
    return q;
}

// What the fragment stage shades by: the element's crown (its centre and
// radii, camera-relative) to work out how deep in it a point lies toward
// the sun, the crown normal, and its colours.
struct PlantVertex {
    vec3 pos;          // camera-relative
    vec3 leafNormal;   // world
    vec3 crownNormal;  // world
    vec3 albedo;
    vec3 glowColor;
    vec4 crown;        // camera-relative centre, the crown's horizontal radius
    vec4 shade;        // x crown height radius, y how deep in its lobe (0 skin .. 1 core), z glow, w density
    float far;         // 0 a leaf .. 1 a daub standing for many
    vec2 leafUV;       // a leaf's corner (across, along: plantLeafCorner); a tube's 0
    bool wood;
};

// A leaf element as plants_pose.comp builds it once for all its corners:
// where its polygon lies (its foot, and its length and width as vectors),
// how it is lit and coloured. Camera-relative.
struct PlantLeafPose {
    vec3 base;         // the polygon's (0, 0) corner, sway and all
    vec3 along;        // to its tip
    vec3 across;       // its width
    vec3 leafNormal;
    vec3 crownNormal;
    vec3 albedo;
    vec3 crownCenter;
    vec2 radii;        // the crown's horizontal and vertical radius
    float far;
    float depth;
    int species;
};

// Element `k` of plant `pi`, drawn `n` strong (the plant's element count).
// False: not drawn.
// A caster (`caster`: a shadow tile's) drawn with fewer elements than its
// crown has lobes spreads them over the lobes and lets each grow past its
// lobe's size, as wide as its share of the crown: a far tree's shadow is
// a few broad daubs (the camera's prefix keeps every lobe instead).
bool plantLeafPose(int pi, int k, float n, bool caster, out PlantLeafPose e) {
    if (float(k) >= ceil(n)) return false;
    Plant p = plantAt(pi);
    PlantSpecies s = plantSpecies(p.species);
    PlantCrown cr = plantCrown(s, p.scale);
    int lobes = clamp(int(s.lobes), 1, PLANT_MAX_LOBES);
    float share = caster ? max(float(lobes) / max(ceil(n), 1.0), 1.0) : 1.0;
    int j = share > 1.0 ? int(float(k) * share) % lobes : k % lobes;
    vec4 lobe = plantLobe(s, cr, p.seed, j);
    uint ks = p.seed * 0x2c1b3c6du + uint(k) * 0x297a2d39u;

    // Where in its lobe: mostly near its skin, facing out from the crown's
    // centre more than from the lobe's, and up toward the light.
    vec3 fromCrown = normalize((lobe.xyz - cr.center) / max(cr.radii, vec3(0.1)) + vec3(0.0, 0.35, 0.0));
    vec3 d = normalize(plantSphere(plantRand(ks, 1u), plantRand(ks, 2u)) + 0.8 * fromCrown);
    float depth = 1.0 - sqrt(plantRand(ks, 3u));       // 0 skin .. 1 core
    // A needle tree's lobe is a branch's tier: a flat spray, wider than it
    // is deep, hanging at its tips.
    vec3 lobeShape = vec3(1.0, mix(1.0, 0.35, s.needle), 1.0);
    vec3 local = lobe.xyz + d * lobeShape * lobe.w * (1.0 - 0.75 * depth);
    local.y -= s.needle * s.droop * lobe.w * dot(d.xz, d.xz) * 0.8;

    // Fewer elements, each bigger: the crown's cover holds as n falls.
    float grow = sqrt(max(s.full, 1.0) / max(n, 1.0));
    float maxGrow = 1.7 * lobe.w * sqrt(share) / max(s.leafLen * p.scale, 1e-3);
    grow = min(grow, max(maxGrow, 1.0));
    float size = s.leafLen * p.scale * grow * (0.75 + 0.5 * plantRand(ks, 4u));
    if (float(k) >= floor(n)) size *= fract(n);
    e.far = clamp((grow - 1.0) / 6.0, 0.0, 1.0);

    // Its facing: about the crown's outward normal, scattered; daubs turn
    // toward the eye as they grow, so a few still cover the crown edge-on.
    vec3 crownN = normalize((local - cr.center) / max(cr.radii * cr.radii, vec3(0.01)));
    vec3 scatter = plantSphere(plantRand(ks, 5u), plantRand(ks, 6u));
    vec3 nrm = normalize(mix(crownN, scatter, mix(0.65, 0.45, s.needle)));
    vec3 worldBase = plantToWorld(p, local);
    vec3 toEye = normalize(camera.eyeWorld.xyz - worldBase);
    vec3 nW = normalize(mix(plantDirToWorld(p, nrm), toEye, 0.85 * e.far));
    // Along the leaf: outward and down by its droop, square to its normal.
    vec3 alongW = plantDirToWorld(p, normalize(d + vec3(0.0, -1.2 * s.droop, 0.0) + 0.6 * scatter));
    alongW = alongW - nW * dot(alongW, nW);
    if (dot(alongW, alongW) < 1e-6) alongW = abs(nW.y) < 0.9 ? cross(nW, vec3(0.0, 1.0, 0.0)) : vec3(1.0, 0.0, 0.0);
    alongW = normalize(alongW);
    vec3 acrossW = cross(nW, alongW);

    // The wind: the trunk's bend at the lobe's height, and the leaf's flutter.
    float h = s.height * p.scale;
    vec3 sway = plantSway(p, local.y, h, float(j) * 0.7);
    float t = camera.windParams.x;
    float flutter = sin(t * (6.0 + 5.0 * plantRand(ks, 7u)) + 6.2831853 * plantRand(ks, 8u)) * (1.0 - e.far);
    vec4 wind = sceneWind(worldBase - camera.eyeWorld.xyz);
    vec3 nF = normalize(nW + alongW * flutter * (0.15 + 0.25 * wind.w) * (1.0 - s.needle * 0.7));

    float width = size * mix(s.leafWidth, 0.5, e.far);
    // The element's centre sits on its point: the leaf hangs from a petiole
    // behind it, the daub is centred.
    e.base = worldBase + sway - alongW * size * mix(0.15, 0.5, e.far) - camera.eyeWorld.xyz;
    e.along = alongW * size;
    e.across = acrossW * width;
    e.leafNormal = nF;
    // The form it is lit as: its lobe's round, on the crown's (a crown of
    // lobes reads as a cluster of lit domes, each with its shaded side).
    e.crownNormal = plantDirToWorld(p, normalize(0.6 * d + 0.4 * crownN));

    // Its colour: the species' light and dark mixed by how deep in the
    // crown it is and how far up, shifted per plant and per lobe.
    float hp = (plantRand(p.seed, 21u) - 0.5) * s.hueVar + 0.35 * p.tint;
    float hl = (plantRand(p.seed, uint(j) * 7u + 4u) - 0.5) * s.hueVar;
    float up = clamp((local.y - cr.center.y) / max(cr.radii.y, 0.1) * 0.5 + 0.5, 0.0, 1.0);
    // A daub standing for many leaves is their mean: its own depth and
    // jitter fade with how many it stands for, so a far crown is its form's
    // light and shade, not a speckle of single leaves.
    float near_ = 1.0 - e.far;
    vec3 col = mix(s.dark, s.light, clamp(0.35 + 0.45 * up - 0.4 * mix(0.35, depth, near_) + hl, 0.0, 1.0));
    col *= 1.0 + hp;
    col *= vec3(1.0 + 0.6 * hl, 1.0, 1.0 - 0.8 * hl);   // warmer or cooler
    col *= 1.0 + (0.2 * plantRand(ks, 9u) - 0.1) * near_;
    e.albedo = max(col, vec3(0.0));
    e.crownCenter = plantToWorld(p, cr.center) + sway - camera.eyeWorld.xyz;
    e.radii = vec2(cr.radii.x, cr.radii.y);
    e.depth = depth;
    e.species = p.species;
    return true;
}

// A pose packed for plants_pose.comp's buffer (set 3 binding 3): 4 uvec4s.
//   [0] base (floats), half(far, depth)
//   [1] half(along.xy), half(along.z, across.x), half(across.yz), species + 1 (0: not drawn)
//   [2] half(leafNormal.xy), half(leafNormal.z, crownNormal.x), half(crownNormal.yz), half(albedo.rg)
//   [3] half(crown centre - base: xy), half(its z, albedo.b), half(radii), 0
#define PLANT_POSE_UVEC4 4

void plantPackPose(PlantLeafPose e, bool drawn, out uvec4 a, out uvec4 b, out uvec4 c, out uvec4 d) {
    vec3 dc = e.crownCenter - e.base;
    a = uvec4(floatBitsToUint(e.base), packHalf2x16(vec2(e.far, e.depth)));
    b = uvec4(packHalf2x16(e.along.xy), packHalf2x16(vec2(e.along.z, e.across.x)), packHalf2x16(e.across.yz),
              drawn ? uint(e.species + 1) : 0u);
    c = uvec4(packHalf2x16(e.leafNormal.xy), packHalf2x16(vec2(e.leafNormal.z, e.crownNormal.x)),
              packHalf2x16(e.crownNormal.yz), packHalf2x16(e.albedo.rg));
    d = uvec4(packHalf2x16(dc.xy), packHalf2x16(vec2(dc.z, e.albedo.b)), packHalf2x16(e.radii), 0u);
}

#ifdef PLANT_POSES_READ
layout(std430, set = 3, binding = 3) readonly buffer PlantPoses {
    uvec4 poses[];
} plantPoses;

// Where pose slot `slot`'s corner `c` lies (camera-relative), and the
// corner in the leaf's own frame. False: not drawn.
bool plantPosedCorner(int slot, int c, out vec3 pos, out vec2 q) {
    uvec4 a = plantPoses.poses[slot * PLANT_POSE_UVEC4];
    uvec4 b = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 1];
    if (b.w == 0u) return false;
    vec2 ab = unpackHalf2x16(b.y);
    vec3 along = vec3(unpackHalf2x16(b.x), ab.x);
    vec3 across = vec3(ab.y, unpackHalf2x16(b.z));
    // (The species' needle, its record's 28th float.)
    float needle = plants.records[int(plants.header[0].w) + (int(b.w) - 1) * PLANT_SPECIES_VEC4 + 6].w;
    q = plantLeafCorner(c, needle);
    pos = uintBitsToFloat(a.xyz) + along * q.y + across * q.x;
    return true;
}

// The whole vertex of pose slot `slot`'s corner `c`, for the colour pass.
bool plantPosedLeaf(int slot, int c, out PlantVertex o) {
    o.wood = false;
    if (!plantPosedCorner(slot, c, o.pos, o.leafUV)) return false;
    uvec4 a = plantPoses.poses[slot * PLANT_POSE_UVEC4];
    uvec4 b = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 1];
    uvec4 cc = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 2];
    uvec4 d = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 3];
    PlantSpecies s = plantSpecies(int(b.w) - 1);
    vec2 fd = unpackHalf2x16(a.w);
    vec2 n1 = unpackHalf2x16(cc.y);
    o.leafNormal = vec3(unpackHalf2x16(cc.x), n1.x);
    o.crownNormal = vec3(n1.y, unpackHalf2x16(cc.z));
    vec2 dz = unpackHalf2x16(d.y);
    o.albedo = vec3(unpackHalf2x16(cc.w), dz.y);
    vec2 radii = unpackHalf2x16(d.z);
    o.crown = vec4(uintBitsToFloat(a.xyz) + vec3(unpackHalf2x16(d.x), dz.x), radii.x);
    o.glowColor = s.glowColor;
    o.shade = vec4(radii.y, fd.y, s.glow, s.density);
    o.far = fd.x;
    return true;
}
#endif

// Tube `limb` of plant `pi` (0 the trunk, 1.. a limb to lobe limb - 1),
// vertex `v` of its PLANT_TUBE_VERTS. False: not drawn.
bool plantWood(int pi, int limb, int v, out PlantVertex o) {
    o.wood = true;
    o.leafUV = vec2(0.0);
    Plant p = plantAt(pi);
    PlantSpecies s = plantSpecies(p.species);
    PlantCrown cr = plantCrown(s, p.scale);
    int lobes = clamp(int(s.lobes), 1, PLANT_MAX_LOBES);
    if (limb > lobes) return false;
    float h = s.height * p.scale;
    float r0 = s.trunk * p.scale;
    vec3 a, b;
    float ra, rb;
    float lagB = 0.0;
    if (limb == 0) {
        // The trunk, up through the crown to most of the way to its top.
        a = vec3(0.0, -0.3, 0.0);
        b = vec3(0.0, mix(cr.center.y, h, 0.6 * (1.0 - s.shape) + 0.35 * s.shape), 0.0);
        ra = r0;
        rb = r0 * 0.35;
    } else {
        vec4 lobe = plantLobe(s, cr, p.seed, limb - 1);
        // From the trunk out to the lobe's heart, rising to it as a limb
        // does (a broadleaf's steeply, a conifer's nearly level), never
        // below the crown's foot.
        float fy = clamp((lobe.y - cr.center.y) / max(cr.radii.y, 0.1) * 0.5 + 0.5, 0.0, 1.0);
        float reach = length(lobe.xz);
        a = vec3(0.0, max(lobe.y - reach * mix(1.1, 0.15, s.shape), h * s.base * 0.8), 0.0);
        b = lobe.xyz - vec3(lobe.x, 0.0, lobe.z) / max(reach, 1e-3) * 0.3 * lobe.w;
        ra = r0 * mix(0.5, 0.28, fy) * (0.6 + 0.6 * lobe.w / max(cr.radii.x, 0.1));
        rb = ra * 0.35;
        lagB = float(limb - 1) * 0.7;
    }
    int quad = v / 6, corner = v % 6;
    const int qx[6] = int[6](0, 1, 1, 0, 1, 0);
    const int qy[6] = int[6](0, 0, 1, 0, 1, 1);
    int side = quad + qx[corner];
    float along = float(qy[corner]);
    float ang = 6.2831853 * float(side) / float(PLANT_TUBE_SIDES);
    vec3 axis = normalize(b - a);
    vec3 s1 = normalize(abs(axis.y) < 0.95 ? cross(axis, vec3(0.0, 1.0, 0.0)) : vec3(1.0, 0.0, 0.0));
    vec3 s2 = cross(axis, s1);
    vec3 radial = cos(ang) * s1 + sin(ang) * s2;
    vec3 at = mix(a, b, along) + radial * mix(ra, rb, along);
    vec3 sway = plantSway(p, at.y, h, along > 0.5 ? lagB : 0.0) * (limb == 0 ? 1.0 : 1.0);
    vec3 world = plantToWorld(p, at) + sway;
    o.pos = world - camera.eyeWorld.xyz;
    o.leafNormal = plantDirToWorld(p, radial);
    o.crownNormal = o.leafNormal;
    float hp = (plantRand(p.seed, 22u) - 0.5) * 0.3;
    o.albedo = s.bark * (1.0 + hp);
    o.glowColor = vec3(0.0);
    o.crown = vec4(plantToWorld(p, cr.center) - camera.eyeWorld.xyz, cr.radii.x);
    o.shade = vec4(cr.radii.y, 0.0, 0.0, s.density);
    o.far = 0.0;
    return true;
}
