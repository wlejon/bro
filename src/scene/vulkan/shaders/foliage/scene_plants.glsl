// Plants (scene/nodes/plants_node.h; drawn by scene_mesh_drawer_plants.cpp):
// every plant is built where it stands from its record, its species' and
// its world's plant module (scene_plant_layer.glsl), nothing stored per
// element. Its foliage is ELEMENTS, opaque polygons (no alpha test: the
// depth test rejects what is hidden before it shades); its wood TUBES.
//
// One representation at every distance: a plant drawn with N elements
// draws the first N of its module's one sequence, each larger as N falls
// so the crown stays covered; the last grows in with N's fraction, so
// nothing pops.
//
// Set 3 binding 1 (the procedural slot): the node's records
// (scene_plant_records.glsl).
// Set 3 binding 2: this frame's chunks, written by the plan
// (plants_plan.comp; scene_plants_gpu_plan.cpp), an instance of a draw
// each: plant, first element, the plant's element count N (a wood chunk:
// its tubes), 1 for a shadow view's caster else 0; a chunk of crown discs
// (shadow_plants.vert): its first disc record, how many, 0, 2. A disc
// record (in the same buffer): plant, 0, 0, 0. A packed chunk, plants of
// few elements sharing its slots: its first packed record, how many, the
// slots each, 3 (4 a caster's); a packed record: plant, its element count,
// 0, 0.
//
// A shader including this includes its module last (foliage/plant_module.glsl,
// a world's in its place), after everything the module may use.

#include "foliage/scene_plant_records.glsl"
#include "foliage/scene_plant_layer.glsl"

layout(std430, set = 3, binding = 2) readonly buffer PlantChunks {
    vec4 chunks[];
} plantChunks;

// A leaf chunk's elements (the plan's PP_LEAVES_PER_CHUNK).
#define PLANT_LEAVES_PER_CHUNK 32

// The element polygon: base, two pairs up its sides and the tip, as 4
// triangles, drawn indexed (the drawer's leafIndices: 0 2 1, 1 2 4, 1 4 3,
// 3 4 5), PLANT_LEAF_POINTS a polygon.
#define PLANT_LEAF_VERTS 12
#define PLANT_LEAF_POINTS 6
// A limb or the trunk: a tapered tube of 6 sides, one span (6 quads).
#define PLANT_TUBE_SIDES 6
#define PLANT_TUBE_VERTS (PLANT_TUBE_SIDES * 6)

// A random unit vector from two uniforms.
vec3 plantSphere(float a, float b) {
    float z = 2.0 * a - 1.0;
    float r = sqrt(max(1.0 - z * z, 0.0));
    float t = 6.2831853 * b;
    return vec3(r * cos(t), z, r * sin(t));
}

// What the fragment stage shades by: the element's crown (its centre and
// radii, camera-relative) to work out how deep in it a point lies toward
// the sun, the crown normal, and its colours.
struct PlantVertex {
    vec3 pos;          // camera-relative
    vec3 leafNormal;   // world
    vec3 crownNormal;  // world
    vec3 albedo;
    vec4 crown;        // camera-relative centre, the crown's horizontal radius
    vec2 shade;        // x the crown's height radius, y how deep in its lobe (0 skin .. 1 core)
    float far;         // 0 a leaf .. 1 a daub standing for many
    vec2 leafUV;       // an element's point (plantLeafPoint); a tube's 0
    int species;
    bool wood;
};

// A pose packed for plants_pose.comp's buffer (set 3 binding 3): 4 uvec4s.
//   [0] base (floats), half(far, depth)
//   [1] half(along.xy), half(along.z, across.x), half(across.yz), species + 1 (0: not drawn)
//   [2] half(leafNormal.xy), half(leafNormal.z, crownNormal.x), half(crownNormal.yz), half(albedo.rg)
//   [3] half(crown centre - base: xy), half(its z, albedo.b), half(radii), 0
#define PLANT_POSE_UVEC4 4

#ifdef PLANT_BUILD
// Element `k` of plant `pi`, drawn `n` strong (the plant's element count),
// as its module builds it. False: not drawn.
bool plantElementAt(int pi, int k, float n, bool caster, out PlantElement e, out PlantCrown cr, out int species) {
    e = PlantElement(vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0), vec3(0.0),
                     0.0, 0.0);
    species = 0;
    if (float(k) >= ceil(n)) return false;
    Plant p = plantAt(pi);
    PlantSpecies s = plantSpecies(p.species);
    cr = plantCrown(s, p.scale);
    species = p.species;
    PlantElementIn in_ = PlantElementIn(k, n, caster, sqrt(max(s.full, 1.0) / max(n, 1.0)),
                                        float(k) >= floor(n) ? fract(n) : 1.0);
    return plantElement(p, s, cr, in_, e);
}

void plantPackPose(PlantElement e, PlantCrown cr, int species, bool drawn, out uvec4 a, out uvec4 b, out uvec4 c,
                   out uvec4 d) {
    vec3 dc = e.crownCenter - e.base;
    a = uvec4(floatBitsToUint(e.base), packHalf2x16(vec2(e.far, e.depth)));
    b = uvec4(packHalf2x16(e.along.xy), packHalf2x16(vec2(e.along.z, e.across.x)), packHalf2x16(e.across.yz),
              drawn ? uint(species + 1) : 0u);
    c = uvec4(packHalf2x16(e.leafNormal.xy), packHalf2x16(vec2(e.leafNormal.z, e.crownNormal.x)),
              packHalf2x16(e.crownNormal.yz), packHalf2x16(e.albedo.rg));
    d = uvec4(packHalf2x16(dc.xy), packHalf2x16(vec2(dc.z, e.albedo.b)), packHalf2x16(cr.radii.xy), 0u);
}
#endif

#ifdef PLANT_POSES_READ
layout(std430, set = 3, binding = 3) readonly buffer PlantPoses {
    uvec4 poses[];
} plantPoses;

// Where pose slot `slot`'s point `c` lies (camera-relative), and the point
// in the element's own frame. False: not drawn.
bool plantPosedCorner(int slot, int c, out vec3 pos, out vec2 q) {
    uvec4 a = plantPoses.poses[slot * PLANT_POSE_UVEC4];
    uvec4 b = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 1];
    if (b.w == 0u) return false;
    vec2 ab = unpackHalf2x16(b.y);
    vec3 along = vec3(unpackHalf2x16(b.x), ab.x);
    vec3 across = vec3(ab.y, unpackHalf2x16(b.z));
    q = plantLeafPoint(int(b.w) - 1, c);
    pos = uintBitsToFloat(a.xyz) + along * q.y + across * q.x;
    return true;
}

// The whole vertex of pose slot `slot`'s point `c`, for the colour pass.
bool plantPosedLeaf(int slot, int c, out PlantVertex o) {
    o.wood = false;
    if (!plantPosedCorner(slot, c, o.pos, o.leafUV)) return false;
    uvec4 a = plantPoses.poses[slot * PLANT_POSE_UVEC4];
    uvec4 b = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 1];
    uvec4 cc = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 2];
    uvec4 d = plantPoses.poses[slot * PLANT_POSE_UVEC4 + 3];
    vec2 fd = unpackHalf2x16(a.w);
    vec2 n1 = unpackHalf2x16(cc.y);
    o.leafNormal = vec3(unpackHalf2x16(cc.x), n1.x);
    o.crownNormal = vec3(n1.y, unpackHalf2x16(cc.z));
    vec2 dz = unpackHalf2x16(d.y);
    o.albedo = vec3(unpackHalf2x16(cc.w), dz.y);
    vec2 radii = unpackHalf2x16(d.z);
    o.crown = vec4(uintBitsToFloat(a.xyz) + vec3(unpackHalf2x16(d.x), dz.x), radii.x);
    o.shade = vec2(radii.y, fd.y);
    o.far = fd.x;
    o.species = int(b.w) - 1;
    return true;
}
#endif

// Tube `limb` of plant `pi` (0 the trunk, 1.. its limbs), vertex `v` of
// its PLANT_TUBE_VERTS, as its module shapes it. False: not drawn.
bool plantWood(int pi, int limb, int v, out PlantVertex o) {
    o.wood = true;
    o.leafUV = vec2(0.0);
    Plant p = plantAt(pi);
    PlantSpecies s = plantSpecies(p.species);
    o.species = p.species;
    PlantCrown cr = plantCrown(s, p.scale);
    if (limb > clamp(s.limbs, 0, PLANT_MAX_LIMBS)) return false;
    PlantLimb l;
    if (!plantLimb(p, s, cr, limb, l)) return false;
    int quad = v / 6, corner = v % 6;
    const int qx[6] = int[6](0, 1, 1, 0, 1, 0);
    const int qy[6] = int[6](0, 0, 1, 0, 1, 1);
    int side = quad + qx[corner];
    float along = float(qy[corner]);
    float ang = 6.2831853 * float(side) / float(PLANT_TUBE_SIDES);
    vec3 axis = normalize(l.b - l.a);
    vec3 s1 = normalize(abs(axis.y) < 0.95 ? cross(axis, vec3(0.0, 1.0, 0.0)) : vec3(1.0, 0.0, 0.0));
    vec3 s2 = cross(axis, s1);
    vec3 radial = cos(ang) * s1 + sin(ang) * s2;
    vec3 at = mix(l.a, l.b, along) + radial * mix(l.ra, l.rb, along);
    vec3 world = plantToWorld(p, at) + plantSway(p, s, at.y, along > 0.5 ? l.lag : 0.0);
    o.pos = world - camera.eyeWorld.xyz;
    o.leafNormal = plantDirToWorld(p, radial);
    o.crownNormal = o.leafNormal;
    o.albedo = l.albedo;
    o.crown = vec4(plantToWorld(p, cr.center) - camera.eyeWorld.xyz, cr.radii.x);
    o.shade = vec2(cr.radii.y, 0.0);
    o.far = 0.0;
    return true;
}
