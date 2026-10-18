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
//   specimens  8 records: (segStart, segCount, clumpStart, clumpCount) and
//              (leafStart, leafCount, crown clump, mean clump weight) as
//              uint bits but the last; (crown.xyz, crown radius across);
//              (crown radius up, mean clump radius, height, first clump
//              block's record as uint bits); its leaves' tint (rgb, its
//              clumps' flatness); its bark's tint (rgb, its leaves' gloss);
//              its blooms' colour (rgb, linear, absolute: not a tint) and
//              form (uint bits: 0 none, 1 flower, 2 head, 3 spike;
//              broflora's BloomForm); its broad leaf (width over length, 0
//              the card's; tip, 0 pointed, 1 rounded; teeth a side; tooth
//              depth, of the half-width); its sprays' brush, its broad
//              leaf's petiole (of its blade's length) and phyllotaxis
//              (0 alternate, 1 opposite, 2 spiral), 0
//   segments   7 records: (from.xyz, rFrom), (to.xyz, rTo), (oct tangent at
//              from, at to), then its vegetation record (4)
//   clumps     6 records: (centre.xyz, radius), (twig dir.xyz, leaf count),
//              then its vegetation record (4), whose third's z is the
//              first placement key of its leaves (uint bits: a broad
//              leaf's place on its shoot, scene_forest_shoot.glsl)
//   blocks     2 records per run of FOREST_CLUMP_BLOCK of a specimen's
//              clumps that lie together: the sphere round their ellipsoids
//              (centre.xyz, radius), and the clumps (the specimen's own
//              indices, 16 bits each, two a float, 0xffff past the last)
//   leaves     2 a record: (clump, placement key) as uint bits
//   instances  2 records: (position.xyz, scale), (cos yaw, sin yaw,
//              specimen as uint bits, seed)
//   blooms     2 records: (position.xyz: its twig's tip, size), (its twig's
//              outward direction.xyz, its twig's segment, an index into the
//              segments above, as uint bits); a specimen's in a run. The
//              header has no room for their first record: a bloom draw
//              entry carries it as its `param`
// All of a specimen's records are relative to where it stands.
//
// Set 3 binding 2 holds this frame's draw entries (the drawer's choice of
// what to draw of which tree), 3 records each:
//   e0  (instance, first, end, param) as uint bits: the run of leaves,
//       segments, clumps or blooms the entry draws [first, end), and the
//       leaf mesh's vertex count (leaves), the tube's sides (bark) or the
//       blooms' first record (blooms)
//   e1  (widening, keep end, tail, fade): leaves are drawn `widening` times
//       their size, shrinking to nothing over the `tail` leaves before
//       `keep end` (so the count thins without popping); `fade` is the
//       dithered cross-fade code mesh.frag reads (ForestNode::fadeCode)
//   e2  (slot, 0, 0, 0) as uint bits: a leaf entry in colour, its first
//       leaf's pose in binding 3 (forest_leaf_pose.comp; -1: none)
//
// Set 3 binding 3 holds this frame's leaf poses (FOREST_POSE_RECORDS uvec4s a
// leaf, forestPoseWrite / forestPose below): the compute pass poses each
// leaf the colour draws keep once, and their vertices read it.

layout(std430, set = 3, binding = 2) readonly buffer ForestEntries {
    vec4 records[];
} forestEntries;

#ifdef FOREST_POSE_WRITE
layout(std430, set = 3, binding = 3) writeonly buffer ForestPoses {
    uvec4 records[];
} forestPoses;
#else
layout(std430, set = 3, binding = 3) readonly buffer ForestPoses {
    uvec4 records[];
} forestPoses;
#endif

struct ForestEntry {
    int instance;
    int first;
    int end;
    int param;
    float widening;
    float keepEnd;
    float tail;
    float fade;
    int slot;
};

ForestEntry forestEntry(int i) {
    uvec4 a = floatBitsToUint(forestEntries.records[i * 3]);
    vec4 b = forestEntries.records[i * 3 + 1];
    int slot = int(floatBitsToUint(forestEntries.records[i * 3 + 2].x));
    return ForestEntry(int(a.x), int(a.y), int(a.z), int(a.w), b.x, b.y, b.z, b.w, slot);
}

// A leaf posed (forest_leaf_pose.comp): where its centre is now
// (camera-relative), the matrix that takes its mesh's points (after the
// fold) to world offsets from there, swayed; its fold (each half turned
// about its stem, +fold on the +x half, -fold on the -x half, as cos and
// sin); its crown shading (scene_vegetation.glsl's vegCrownShade) and tint;
// its seed. A cluster's lobe: the way its leaves face before their tilt,
// its sphere (centre less the leaf's, unswayed: the lobe moves with the
// leaf) and radius, its leaves' optical depth a unit length and its height
// over its width. 4 uvec4s.
const int FOREST_POSE_RECORDS = 4;
struct ForestPose {
    bool alive;
    vec3 at;
    mat3 M;
    vec2 fold;
    vec4 shade;
    vec3 tint;
    float seed;
    vec3 lobeOut;
    vec4 lobeSphere;   // centre less `at`, radius
    float lobeDensity;
    float lobeFlat;
};

#ifdef FOREST_POSE_WRITE
void forestPoseWrite(int slot, ForestPose p) {
    int b = slot * FOREST_POSE_RECORDS;
    if (!p.alive) {
        forestPoses.records[b + 1] = uvec4(0u);
        return;
    }
    forestPoses.records[b] = uvec4(floatBitsToUint(p.at), packSnorm4x8(vec4(p.shade.xyz, -p.shade.w)));
    forestPoses.records[b + 1] = uvec4(packHalf2x16(p.M[0].xy), packHalf2x16(vec2(p.M[0].z, p.M[1].x)),
                                       packHalf2x16(p.M[1].yz), packHalf2x16(p.M[2].xy));
    forestPoses.records[b + 2] = uvec4(packHalf2x16(vec2(p.M[2].z, 1.0)), packHalf2x16(p.fold),
                                       packHalf2x16(p.tint.xy),
                                       packHalf2x16(vec2(p.tint.z, 0.0)) | (uint(clamp(p.seed, 0.0, 1.0) * 65535.0 + 0.5) << 16));
    forestPoses.records[b + 3] = uvec4(packSnorm4x8(vec4(p.lobeOut, 0.0)), packHalf2x16(p.lobeSphere.xy),
                                       packHalf2x16(p.lobeSphere.zw), packHalf2x16(vec2(p.lobeDensity, p.lobeFlat)));
}
#else
ForestPose forestPose(int slot) {
    int b = slot * FOREST_POSE_RECORDS;
    ForestPose p;
    uvec4 r1 = forestPoses.records[b + 1];
    p.alive = r1.x != 0u;
    if (!p.alive) return p;
    uvec4 r0 = forestPoses.records[b];
    uvec4 r2 = forestPoses.records[b + 2];
    p.at = uintBitsToFloat(r0.xyz);
    vec4 s = unpackSnorm4x8(r0.w);
    p.shade = vec4(s.xyz, -s.w);
    vec2 a = unpackHalf2x16(r1.x), b1 = unpackHalf2x16(r1.y), c = unpackHalf2x16(r1.z), d = unpackHalf2x16(r1.w);
    vec2 e = unpackHalf2x16(r2.x);
    p.M = mat3(vec3(a, b1.x), vec3(b1.y, c), vec3(d, e.x));
    p.fold = unpackHalf2x16(r2.y);
    p.tint = vec3(unpackHalf2x16(r2.z), unpackHalf2x16(r2.w & 0xffffu).x);
    p.seed = float(r2.w >> 16) / 65535.0;
    return p;
}
// A cluster's lobe (the pose's last record).
void forestPoseLobe(int slot, inout ForestPose p) {
    uvec4 r3 = forestPoses.records[slot * FOREST_POSE_RECORDS + 3];
    p.lobeOut = unpackSnorm4x8(r3.x).xyz;
    p.lobeSphere = vec4(unpackHalf2x16(r3.y), unpackHalf2x16(r3.z));
    vec2 df = unpackHalf2x16(r3.w);
    p.lobeDensity = df.x;
    p.lobeFlat = df.y;
}
#endif

int forestBase(int k) { return int(floatBitsToUint(procedural.header[2][k])); }
int forestSpecimenRecord(int s) { return forestBase(0) + s * FOREST_SPECIMEN_RECORDS; }

// Specimen `s`'s tint on the node's colour: its leaves' or its bark's.
vec3 forestSpecimenTint(int s, bool bark) { return procedural.records[forestSpecimenRecord(s) + (bark ? 5 : 4)].xyz; }

// Specimen `s`'s broad leaf: (width over length, tip, teeth a side, tooth
// depth), all 0 for the node's own; and its gloss (0..1).
vec4 forestSpecimenLeaf(int s) { return procedural.records[forestSpecimenRecord(s) + 7]; }
float forestSpecimenGloss(int s) { return procedural.records[forestSpecimenRecord(s) + 5].w; }
// Specimen `s`'s sprays' brush (broflora's Species::sprayBrush).
float forestSpecimenBrush(int s) { return procedural.records[forestSpecimenRecord(s) + 8].x; }
// Specimen `s`'s broad leaf on its shoot (scene_forest_shoot.glsl): its
// petiole's length over its blade's (0: sessile) and its phyllotaxis (0
// alternate, 1 opposite, 2 spiral; broflora's Species::petiole and
// Species::phyllotaxis).
vec2 forestSpecimenShoot(int s) { return procedural.records[forestSpecimenRecord(s) + 8].yz; }

// Specimen `s`'s blooms: their colour (rgb, linear) and form (w: 0 none, 1
// flower, 2 head, 3 spike).
vec4 forestSpecimenBloom(int s) {
    vec4 r = procedural.records[forestSpecimenRecord(s) + 6];
    return vec4(r.xyz, float(floatBitsToUint(r.w)));
}

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
// A piece of wood between its joints `a` and `b`, its way there `ta` and
// `tb` (unit; the way the pieces meeting at a joint share): a cubic Hermite
// curve through both joints, leaving and arriving along those ways, so a
// branch or twig of many pieces is one curve, its way continuous through
// every joint, not a polyline kinked at each (scene_forest_tube.glsl's bark
// rings, scene_forest_shoot.glsl's twigs). The point `f` (0..1) along it
// and its way there. Its ends' ways pull it FOREST_HERMITE_PULL of the
// chord's length: at the whole of it a piece whose ends turn far from its
// chord (a zigzag twig's) swings out into a hook.
const float FOREST_HERMITE_PULL = 0.8;
vec3 forestHermite(vec3 a, vec3 ta, vec3 b, vec3 tb, float f, out vec3 tangent) {
    float L = FOREST_HERMITE_PULL * length(b - a);
    vec3 m0 = ta * L, m1 = tb * L;
    float f2 = f * f, f3 = f2 * f;
    vec3 d = (6.0 * f2 - 6.0 * f) * (a - b) + (3.0 * f2 - 4.0 * f + 1.0) * m0 + (3.0 * f2 - 2.0 * f) * m1;
    tangent = dot(d, d) > 1e-14 ? normalize(d) : normalize(b - a + vec3(1e-7));
    return a + (3.0 * f2 - 2.0 * f3) * (b - a) + (f3 - 2.0 * f2 + f) * m0 + (f3 - f2) * m1;
}

int forestSegmentRecord(int g) { return forestBase(1) + g * 7; }
int forestClumpRecord(int c) { return forestBase(2) + c * 6; }

// Leaf `i`: its clump (global) and its placement key.
ivec2 forestLeaf(int i) {
    vec4 r = procedural.records[forestBase(3) + i / 2];
    uvec2 u = floatBitsToUint((i & 1) == 0 ? r.xy : r.zw);
    return ivec2(u);
}

// A needle or scale-leaf spray of instance `i`'s specimen, placed by
// clumpLeafFrame in clump `clump` (global) about its lobe's centre `lc`
// (radius `lr`), turned by the specimen's brush (forestSpecimenBrush): at 1
// its length runs out from the lobe's middle along its shoot and swept up,
// so the clump is a brush of needles radiating from the shoot's end, not
// sprays lying round a ball; and it is drawn in toward the lobe's middle,
// so the brush reaches about as far as the lobe the clump tier draws. The
// leaf mesh is centred, its length +Z (R's third column), across +X, its
// normal +Y.
void forestSprayBrush(int i, int clump, vec3 lc, float lr, inout vec3 P, inout mat3 R, inout vec3 F) {
    if (forestLeafForm() == 0) return;
    float b = clamp(forestSpecimenBrush(forestInstanceSpecimen(i)), 0.0, 1.0);
    if (b <= 0.0) return;
    float scale = length(R[2]);
    vec3 N = R[1] / max(length(R[1]), 1e-9);
    vec3 o = P - lc;
    vec3 out1 = dot(o, o) > 1e-12 ? normalize(o) : vec3(0.0, 1.0, 0.0);
    vec3 tw = procedural.records[forestClumpRecord(clump) + 1].xyz;
    tw = dot(tw, tw) > 1e-12 ? normalize(tw) : out1;
    vec3 Fb = normalize(out1 + 0.6 * tw + vec3(0.0, 0.9, 0.0));
    vec3 Fn = normalize(mix(F, Fb, b));
    vec3 Nn = N - Fn * dot(N, Fn);
    Nn = dot(Nn, Nn) > 1e-10 ? normalize(Nn) : perpendicularUnit(Fn);
    R = mat3(cross(Fn, Nn) * scale, Nn * scale, Fn * scale);
    F = Fn;
    P = lc + o * (1.0 - 0.45 * b);
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
// A colour leaf within FOREST_NEAR_SHRINK.y of the eye shrinks to its middle,
// gone by .x (m): the eye passing through a crown. Closer than
// scene_vegetation.glsl's metre: a forest leaf hangs on its twig, and one
// shrunk half a metre off reads as a tiny leaf floating loose by full-size
// ones; only the leaves the eye all but touches give way.
const vec2 FOREST_NEAR_SHRINK = vec2(0.1, 0.4);
