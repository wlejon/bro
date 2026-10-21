// A plants node's records (scene/nodes/plants_node.h, PlantsNode::gpuData),
// uploaded when they change, never per frame, and what a species and a
// plant are read as. The plants' draws bind them at set 3 binding 1 (the
// procedural slot; scene_plants.glsl); another reader defines
// PLANT_RECORDS_SET and PLANT_RECORDS_BINDING before including this.
//   header[0]  plants, species, 0, the first species record
//   header[1]  the first plant record, 0, 0, 0
//   header[2]  0, 0, 0, 0
//   header[3]  0, 0, 0, 0
//   records[]  species (PLANT_SPECIES_VEC4 each), plants in cell order (2
//              each: x, y, z, yaw; scale, species, seed (an integer under
//              2^24), tint)

#ifndef PLANT_RECORDS_SET
#define PLANT_RECORDS_SET 3
#define PLANT_RECORDS_BINDING 1
#endif

layout(std430, set = PLANT_RECORDS_SET, binding = PLANT_RECORDS_BINDING) readonly buffer PlantRecords {
    vec4 header[4];
    vec4 records[];
} plants;

#define PLANT_SPECIES_VEC4 8

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
    // (Never past the records: a chunk names a plant of this frame's plan.)
    i = clamp(i, 0, max(int(plants.header[0].x) - 1, 0));
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

// The plant's frame to the world: turned by its yaw about its foot.
vec3 plantToWorld(Plant p, vec3 local) {
    float cs = cos(p.yaw), sn = sin(p.yaw);
    return p.pos + vec3(cs * local.x - sn * local.z, local.y, sn * local.x + cs * local.z);
}
vec3 plantDirToWorld(Plant p, vec3 d) {
    float cs = cos(p.yaw), sn = sin(p.yaw);
    return vec3(cs * d.x - sn * d.z, d.y, sn * d.x + cs * d.z);
}
