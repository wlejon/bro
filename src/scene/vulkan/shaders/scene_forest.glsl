// The forest node (scene/forest_node.h): many instances of a few grown
// specimens, each drawn at the level of detail its distance asks for, every
// level posed by the same sway (scene_vegetation.glsl's vegPose). Needs
// scene_camera.glsl, scene_procedural.glsl (its `procedural` block and the
// clump-mode leaf) and scene_vegetation.glsl.
//
// Set 3 binding 1 holds the node's data, uploaded once (ForestNode::gpuData):
//   header[0]  seed (uint bits), up bias, tilt jitter, shell   (the scatter's)
//   header[1]  base scale, scale jitter, leaf length, flutter fade (widening)
//   header[2]  first record of: specimens, segments, clumps, leaves (uint bits)
//   header[3]  first record of the instances (uint bits); the leaf's width
//              over its length; -, -
//   specimens  4 records: (segStart, segCount, clumpStart, clumpCount) and
//              (leafStart, leafCount, crown clump, mean clump weight) as
//              uint bits but the last; (crown.xyz, crown radius across);
//              (crown radius up, mean clump radius, height, -)
//   segments   7 records: (from.xyz, rFrom), (to.xyz, rTo), (oct tangent at
//              from, at to), then its vegetation record (4)
//   clumps     6 records: (centre.xyz, radius), (twig dir.xyz, leaf count),
//              then its vegetation record (4)
//   leaves     2 a record: (clump, placement key) as uint bits
//   instances  2 records: (position.xyz, scale), (cos yaw, sin yaw,
//              specimen as uint bits, seed)
// All of a specimen's records are relative to where it stands.
//
// Set 3 binding 2 holds this frame's draw entries (the drawer's choice of
// what to draw of which tree), 2 records each:
//   e0  (instance, first, end, param) as uint bits: the run of leaves,
//       segments or clumps the entry draws [first, end), and the leaf
//       mesh's vertex count (leaves) or the tube's sides (bark)
//   e1  (widening, keep end, tail, fade): leaves are drawn `widening` times
//       their size, shrinking to nothing over the `tail` leaves before
//       `keep end` (so the count thins without popping); `fade` is the
//       dithered cross-fade code mesh.frag reads (ForestNode::fadeCode)

layout(std430, set = 3, binding = 2) readonly buffer ForestEntries {
    vec4 records[];
} forestEntries;

struct ForestEntry {
    int instance;
    int first;
    int end;
    int param;
    float widening;
    float keepEnd;
    float tail;
    float fade;
};

ForestEntry forestEntry(int i) {
    uvec4 a = floatBitsToUint(forestEntries.records[i * 2]);
    vec4 b = forestEntries.records[i * 2 + 1];
    return ForestEntry(int(a.x), int(a.y), int(a.z), int(a.w), b.x, b.y, b.z, b.w);
}

int forestBase(int k) { return int(floatBitsToUint(procedural.header[2][k])); }
int forestSpecimenRecord(int s) { return forestBase(0) + s * 4; }
int forestSegmentRecord(int g) { return forestBase(1) + g * 7; }
int forestClumpRecord(int c) { return forestBase(2) + c * 6; }

// Leaf `i`: its clump (global) and its placement key.
ivec2 forestLeaf(int i) {
    vec4 r = procedural.records[forestBase(3) + i / 2];
    uvec2 u = floatBitsToUint((i & 1) == 0 ? r.xy : r.zw);
    return ivec2(u);
}

// Instance `i`: its specimen, its transform (specimen space to node space:
// a turn about +Y, a uniform scale, a position) and a seed in [0, 1).
mat4 forestInstance(int i, out int specimen, out float seed) {
    int b = int(floatBitsToUint(procedural.header[3].x)) + i * 2;
    vec4 p = procedural.records[b];
    vec4 q = procedural.records[b + 1];
    specimen = int(floatBitsToUint(q.z));
    seed = q.w;
    float s = p.w;
    return mat4(vec4(q.x * s, 0.0, -q.y * s, 0.0), vec4(0.0, s, 0.0, 0.0), vec4(q.y * s, 0.0, q.x * s, 0.0),
                vec4(p.xyz, 1.0));
}

// The vegetation record at record `r`: v0, v1, v2 (its anchor `anchor`, its
// weight `along` from its from-end to its to-end), v3.
void forestVegetation(int r, vec3 anchor, float along, out vec4 v0, out vec4 v1, out vec4 v2, out vec4 v3) {
    v0 = procedural.records[r];
    v1 = procedural.records[r + 1];
    vec4 w = procedural.records[r + 2];
    v2 = vec4(anchor, mix(w.x, w.y, along));
    v3 = procedural.records[r + 3];
}

// A leaf of the forest, swayed: the wood's pose where it hangs, its own
// motion (turn, flutter, fold) scaled by `flutter`, and the crown's shading
// (scene_vegetation.glsl's vegetationSwayAxis for a leaf, with the leaf's
// seed hashed with its tree's so two trees of one specimen do not flutter
// in step). `pos` is the vertex's camera-relative rest position.
vec4 forestLeafSway(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, vec3 axisNode, vec3 leafNormal, float flutter,
                    float treeSeed, inout vec3 pos, out mat3 rot) {
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, anchor, clamp(v2.w, 0.0, 1.0));
    float seed = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719)) + treeSeed * 61.7) * 43758.5453);
    rot = vegLeafAmount(pose, w, normalize(mat3(model) * axisNode), leafNormal, pos - anchor, seed, flutter);
    pos = anchor + pose.offset + rot * (pos - anchor);
    return vegCrownShade(model, v1, anchor);
}

// The flutter a leaf keeps at a widening: whole up close, gone by the
// header's flutter fade.
float forestFlutter(float widening) {
    float to = max(procedural.header[1].w, 1.01);
    return 1.0 - smoothstep(1.0, to, widening);
}

const vec4 FOREST_CULLED = vec4(2.0, 2.0, 2.0, 1.0);
