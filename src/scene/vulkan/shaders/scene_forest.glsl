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
//              over its length (a spray's: its needles' cover, as an
//              ellipse); the leaves' flutter; the look as uint bits: leaf
//              form (bits 0-3), limb bob and twig bob in 64ths (8 bits each)
//   specimens  6 records: (segStart, segCount, clumpStart, clumpCount) and
//              (leafStart, leafCount, crown clump, mean clump weight) as
//              uint bits but the last; (crown.xyz, crown radius across);
//              (crown radius up, mean clump radius, height, first clump
//              block's record as uint bits); its leaves' tint (rgb, its
//              clumps' flatness); its bark's tint (rgb, -)
//   segments   7 records: (from.xyz, rFrom), (to.xyz, rTo), (oct tangent at
//              from, at to), then its vegetation record (4)
//   clumps     6 records: (centre.xyz, radius), (twig dir.xyz, leaf count),
//              then its vegetation record (4)
//   blocks     2 records per run of FOREST_CLUMP_BLOCK of a specimen's
//              clumps that lie together: the sphere round their ellipsoids
//              (centre.xyz, radius), and the clumps (the specimen's own
//              indices, 16 bits each, two a float, 0xffff past the last)
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
int forestSpecimenRecord(int s) { return forestBase(0) + s * FOREST_SPECIMEN_RECORDS; }

// Specimen `s`'s tint on the node's colour: its leaves' or its bark's.
vec3 forestSpecimenTint(int s, bool bark) { return procedural.records[forestSpecimenRecord(s) + (bark ? 5 : 4)].xyz; }

// The node's leaf form (header[3].w bits 0-3): 0 broad, 1 needle sprays, 2
// scale-leaf sprays; and the share of its card a spray covers
// (ForestNode::sprayFill).
int forestLeafForm() { return int(floatBitsToUint(procedural.header[3].w) & 15u); }
float forestSprayFill() {
    int f = forestLeafForm();
    return f == 1 ? 0.45 : f == 2 ? 0.47 : 1.0;
}

// The node's look (header[3].w): its plants' limbs and twigs bounce as they
// ring (scene_vegetation.glsl's vegLimbBob / vegTwigBob, which every
// forest vertex stage sets first, so every tier is posed alike).
void forestApplyLook() {
    uint w = floatBitsToUint(procedural.header[3].w);
    vegLimbBob = float((w >> 8) & 255u) / 64.0;
    vegTwigBob = float((w >> 16) & 255u) / 64.0;
}

// Specimen `s`'s clumps' height over their width (specimen record 4's w,
// broflora's Species::clumpFlat: a fir's flat pads of level sprays).
float forestClumpFlat(int s) {
    float f = procedural.records[forestSpecimenRecord(s) + 4].w;
    return f > 0.0 ? f : 1.0;
}
// The specimen instance `i` draws.
int forestInstanceSpecimen(int i) {
    return int(floatBitsToUint(procedural.records[int(floatBitsToUint(procedural.header[3].x)) + i * 2 + 1].z));
}
// Every clump placement (clumpLeafFrame, procClumpLobe) flattened as
// instance `i`'s specimen's clumps are.
void forestApplyClumpFlat(int i) { procClumpFlat = forestClumpFlat(forestInstanceSpecimen(i)); }

// A lobe's CORE (specimen units): its leaves fill the shell under its
// surface (clumpLeafFrame: `shell` of the radius deep, flattened by
// procClumpFlat), `leaves` of them, each turned every way. Where the shell's
// optical depth from the surface in reaches FOREST_CORE_DEPTH nothing behind
// shows: the core is the ellipsoid from there in, drawn solid (the drawer's
// cores, mesh.frag's core sprite) so the leaves and sprays it hides fail the
// depth test rather than being shaded: the core's radius, the lobe's shrunk
// by that depth. 0 when the shell is thinner than that (a lobe the eye sees
// through). `sigma` the leaves' optical depth a unit length.
const float FOREST_CORE_DEPTH = 1.5;
float forestLobeCore(float lr, float leaves, out float sigma) {
    float shell = clamp(procedural.header[0].w, 0.05, 1.0);
    float a = 0.5 * procedural.header[1].z;
    float area = 3.14159265 * a * a * procedural.header[3].y * 0.55;
    float inner = 1.0 - shell;
    float volume = 4.18879 * lr * lr * lr * 0.8 * procClumpFlat * (1.0 - inner * inner * inner);
    sigma = leaves * area / max(volume, 1e-9);
    // Measured across, the way a crown is mostly seen (a flat pad's shell
    // is thinner up and down, but the pads stack).
    float depth = FOREST_CORE_DEPTH / max(sigma, 1e-6);
    if (depth >= shell * lr) return 0.0;
    return lr - depth;
}
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
    return (1.0 - smoothstep(1.0, to, widening)) * procedural.header[3].z;
}

const vec4 FOREST_CULLED = vec4(2.0, 2.0, 2.0, 1.0);
