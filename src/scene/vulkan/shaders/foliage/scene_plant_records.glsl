// A plants node's records (scene/nodes/plants_node.h, PlantsNode::gpuData),
// uploaded when they change, never per frame, and what a species and a
// plant are read as. The plants' draws bind them at set 3 binding 1 (the
// procedural slot; scene_plants.glsl); another reader defines
// PLANT_RECORDS_SET and PLANT_RECORDS_BINDING before including this.
//   header[0]  plants, species, 0, the first species record
//   header[1]  the first plant record, 0, 0, 0
//   header[2]  0, 0, 0, 0
//   header[3]  0, 0, 0, 0
//   records[]  species (PLANT_SPECIES_VEC4 each: the engine's two, then the
//              module's PLANT_DATA_VEC4), plants in cell order (2 each: x, y,
//              z, yaw; scale, species, seed (an integer under 2^24), tint)

#ifndef PLANT_RECORDS_SET
#define PLANT_RECORDS_SET 3
#define PLANT_RECORDS_BINDING 1
#endif

layout(std430, set = PLANT_RECORDS_SET, binding = PLANT_RECORDS_BINDING) readonly buffer PlantRecords {
    vec4 header[4];
    vec4 records[];
} plants;

#define PLANT_SPECIES_VEC4 8
#define PLANT_ENGINE_VEC4 3 // the engine's share of a species (PlantSpecies)
#define PLANT_DATA_VEC4 5   // the module's (PlantsNode::kSpeciesDataFloats / 4)

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

// What the engine knows of a species: its size (the plants' bounds, a far
// caster's disc), its detail (the plan's element and limb counts) and its
// crown as the light sees it (an envelope of a medium: the light through
// it, plants.frag; the canopy in the probe grid, probe_splat.comp). The
// rest is its module's (plantData).
struct PlantSpecies {
    int index;
    float height;      // m, at scale 1
    float base;        // where the crown starts, a fraction of the height
    float radius;      // the crown's radius (m)
    float full;        // elements at full detail
    float leafLen;     // an element's length (m) at full detail
    int limbs;         // tubes besides the trunk (drawn near; far off the trunk alone)
    float shape;       // the envelope: 0 a round crown .. 1 a cone
    float density;     // how dense the crown is to light (1/m)
    vec3 albedo;       // its foliage's mean colour (linear)
};

PlantSpecies plantSpecies(int i) {
    int r = int(plants.header[0].w) + i * PLANT_SPECIES_VEC4;
    vec4 a = plants.records[r], b = plants.records[r + 1], c = plants.records[r + 2];
    PlantSpecies s;
    s.index = i;
    s.height = a.x; s.base = a.y; s.radius = a.z; s.full = a.w;
    s.leafLen = b.x; s.limbs = int(b.y); s.shape = b.z; s.density = b.w;
    s.albedo = c.xyz;
    return s;
}

// The module's vec4 `i` (0..PLANT_DATA_VEC4-1) of species `species`.
vec4 plantData(int species, int i) {
    return plants.records[int(plants.header[0].w) + species * PLANT_SPECIES_VEC4 + PLANT_ENGINE_VEC4 +
                          clamp(i, 0, PLANT_DATA_VEC4 - 1)];
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
// about `center` with `radii`, narrowed toward its top by `shape` (a cone
// at 1). The light and the probes take it as the crown; a module builds its
// crown within it.
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
