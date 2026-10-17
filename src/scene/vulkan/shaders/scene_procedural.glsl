// The segment records a procedural instanced draw (foliage scatter, branch
// tubes) builds its geometry from: set 3 binding 1, the slot skinned meshes
// use binding 0 of for their bone palette. SceneMeshDrawer::prepare writes it.
//
//   header[0..2]   the mode's parameters (see each shader); header[2].z is the
//                  first vegetation record (0: the node carries none)
//   header[3]      scatter: leaves an instance, the leaf mesh's vertex count,
//                  the leaf count (scatterLeafIndex)
//   records[]      2 per segment, then (scatter) the per-leaf segment indices
//                  packed four to a record, then (with vegetation) 4 per
//                  segment: (root.xyz, height), (crown.xyz, crownRadius),
//                  (weightFrom, weightTo, 0, 0), (0, twig, limb, trunk) —
//                  scene_vegetation.glsl

layout(std430, set = 3, binding = 1) readonly buffer ProceduralSegments {
    vec4 header[4];
    vec4 records[];
} procedural;

// The leaf a scatter vertex belongs to: each instance draws header[3].x
// copies of the leaf mesh end to end (InstancedMeshNode::scatterBatchMesh).
// False past the last leaf.
bool scatterLeafIndex(int instanceIndex, int vertexIndex, out int leaf) {
    vec4 h = procedural.header[3];
    leaf = instanceIndex * int(h.x + 0.5) + vertexIndex / max(int(h.y + 0.5), 1);
    return leaf < int(h.z + 0.5);
}

const float PROC_TWO_PI = 6.28318530718;

// Any unit vector perpendicular to unit `t` (bromesh's perpendicularUnit), so
// tube rings and leaf frames share a basis.
vec3 perpendicularUnit(vec3 t) {
    vec3 c = cross(t, vec3(0.0, 1.0, 0.0));
    if (dot(c, c) < 1e-8) c = cross(t, vec3(1.0, 0.0, 0.0));
    return normalize(c);
}

// A unit vector from two floats (octahedral; InstancedMeshNode's octEncode).
vec3 procOctDecode(vec2 e) {
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

// The vertex of a tube draw: segment from the vertex index, `sides` quads a
// segment as two triangles across its rings. Records: (from.xyz, radiusFrom),
// (to.xyz, radiusTo), and with mitred joints a third, (oct(tangentFrom),
// oct(tangentTo)). Header: x sides, y radius scale, z records per segment (3:
// mitred). `seg` and `endT` (0 at from, 1 at to) say where on the skeleton
// the vertex sits. False for a degenerate segment.
//
// A mitred end ring lies in the plane across the bark's direction there (the
// bisector of the two segments meeting), not across this segment, and its
// radial directions come from that direction alone (tubeRingFrame): two
// segments meeting at an angle build the very same ring, so the bark closes
// around the bend with no crack, and its normals there are shared, so the
// kink shades as a bend in the wood rather than a crease.
//
// The ring frame at a tube end: radial directions in the plane across unit
// `t`, the least rotation taking up (+Y) to `t` applied to +X. Neighbouring
// rings with nearly the same `t` get nearly the same frame, so a near-vertical
// trunk does not twist from segment to segment (a frame from cross(t, up)
// spins freely there). Degenerate only for `t` straight down.
void tubeRingFrame(vec3 t, out vec3 e1, out vec3 e2) {
    float c = t.y;
    if (c < -0.9999) {
        e1 = vec3(1.0, 0.0, 0.0);
    } else {
        float k = 1.0 / (1.0 + c);
        e1 = normalize(vec3(c + t.z * t.z * k, -t.x, -t.x * t.z * k));
    }
    e2 = cross(t, e1);
}

bool tubeVertex(int vertexIndex, out vec3 pos, out vec3 radial, out vec3 axisDir, out vec2 uv,
                out int seg, out float endT) {
    int sides = int(procedural.header[0].x);
    int stride = procedural.header[0].z > 2.5 ? 3 : 2;
    int perSeg = sides * 6;
    seg = vertexIndex / perSeg;
    int local = vertexIndex - seg * perSeg;
    int side = local / 6;
    int corner = local - side * 6;

    // corner 0=b0 1=b1 2=t0   3=t0 4=b1 5=t1 (b = from ring, t = to ring)
    bool top = corner == 2 || corner == 3 || corner == 5;
    bool nextSide = corner == 1 || corner == 4 || corner == 5;
    float ang = float(nextSide ? side + 1 : side) / float(sides) * PROC_TWO_PI;
    endT = top ? 1.0 : 0.0;

    vec4 s0 = procedural.records[seg * stride];
    vec4 s1 = procedural.records[seg * stride + 1];
    vec3 axis = s1.xyz - s0.xyz;
    float len = length(axis);
    pos = vec3(0.0);
    radial = vec3(0.0);
    axisDir = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    if (len < 1e-6) return false;
    axisDir = axis / len;
    // The ring's plane: across the joint's direction where one is given (and
    // not too far from this segment's own), else across the segment.
    if (stride == 3) {
        vec4 j = procedural.records[seg * stride + 2];
        vec3 T = procOctDecode(top ? j.zw : j.xy);
        if (dot(axisDir, T) > 0.2) axisDir = T;
    }
    vec3 e1, e2;
    tubeRingFrame(axisDir, e1, e2);
    radial = cos(ang) * e1 + sin(ang) * e2;
    float r = mix(s0.w, s1.w, endT) * procedural.header[0].y;
    pos = (top ? s1.xyz : s0.xyz) + r * radial;
    uv = vec2(ang / PROC_TWO_PI, endT);
    return true;
}

// Chris Wellons' triple32-lite.
uint procHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

vec3 procNormalizeOr(vec3 v, vec3 fallback) {
    return dot(v, v) < 1e-8 ? normalize(fallback) : normalize(v);
}

// Rodrigues rotation of v about a unit axis.
vec3 procRotAxis(vec3 v, vec3 axis, float ang) {
    float c = cos(ang), s = sin(ang);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

// Foliage scatter: leaf `leaf`'s frame, synthesised from the segment records
// as bromesh::placeLeavesOnBranches places one — the same up bias, tilt /
// roll / scale jitter and leaf frame (+Z tip, +Y card normal, +X side). The
// random stream is a 32-bit hash keyed on the instance index, so placement is
// deterministic though leaf for leaf unlike the CPU's.
//
// Records: (from.xyz, radius), (dir.xyz, leaf count) per segment, then each
// leaf's segment index, four to a record from header[2].y. Header:
//   [0] seed (uint bits), up bias, tilt jitter, roll jitter
//   [1] base scale, scale jitter, scale by radius, reference radius
//   [2] density falloff, first leaf-index record, first vegetation record
//
// `R` takes the leaf mesh (scaled) into node space at `P`, the point on the
// segment it hangs from; `F` is its unit tip direction. False for a leaf with
// no extent.
//
// Clump mode (header[2].w = 1, scatterClumpLeaf below) reads the records as
// leaf clumps instead.
bool scatterClumpLeaf(int leaf, int clump, out vec3 P, out mat3 R, out vec3 F);

bool scatterLeaf(int leaf, out vec3 P, out mat3 R, out vec3 F, out int seg, out float along) {
    vec4 h0 = procedural.header[0];
    vec4 h1 = procedural.header[1];
    vec4 h2 = procedural.header[2];
    float upBias = h0.y, tiltJitter = h0.z, rollJitter = h0.w;
    float baseScale = h1.x, scaleJitter = h1.y, scaleByRadius = h1.z, refRadius = h1.w;
    float densityFalloff = h2.x;

    P = vec3(0.0);
    R = mat3(1.0);
    F = vec3(0.0, 0.0, 1.0);
    seg = int(procedural.records[int(h2.y) + leaf / 4][leaf % 4] + 0.5);
    along = 0.0;
    if (h2.w > 0.5) {
        along = 1.0;
        return scatterClumpLeaf(leaf, seg, P, R, F);
    }
    vec4 t0 = procedural.records[seg * 2];
    vec4 t1 = procedural.records[seg * 2 + 1];
    vec3 from = t0.xyz;
    float radius = t0.w;
    vec3 dir = t1.xyz;

    float len = length(dir);
    if (len < 1e-6) return false;
    vec3 T = dir / len;

    uint base = procHash(floatBitsToUint(h0.x) ^ procHash(uint(leaf) + 1u));
    #define DRAW(k) (float(procHash(base + uint(k) * 0x9e3779b9u) >> 8) * (1.0 / 16777216.0))

    float u = DRAW(0);
    along = densityFalloff > 0.0 ? 1.0 - pow(1.0 - u, 1.0 + densityFalloff) : u;
    P = from + dir * along;

    float phi = DRAW(1) * PROC_TWO_PI;
    vec3 e1 = perpendicularUnit(T);
    vec3 e2 = cross(T, e1);
    vec3 Rd = e1 * cos(phi) + e2 * sin(phi);

    const vec3 worldUp = vec3(0.0, 1.0, 0.0);
    F = procNormalizeOr(Rd * (1.0 - upBias) + worldUp * upBias, Rd);
    if (tiltJitter > 0.0) {
        float tiltAng = (DRAW(2) * 2.0 - 1.0) * tiltJitter;
        F = normalize(procRotAxis(F, procNormalizeOr(cross(F, T), e2), tiltAng));
    }

    vec3 sideAxis = cross(F, worldUp);
    if (dot(sideAxis, sideAxis) < 1e-8) sideAxis = cross(F, vec3(1.0, 0.0, 0.0));
    sideAxis = normalize(sideAxis);
    vec3 N = normalize(cross(sideAxis, F));
    if (rollJitter > 0.0) {
        float rollAng = (DRAW(3) * 2.0 - 1.0) * rollJitter;
        sideAxis = normalize(procRotAxis(sideAxis, F, rollAng));
        N = normalize(procRotAxis(N, F, rollAng));
    }

    float jitter = (DRAW(4) * 2.0 - 1.0) * scaleJitter;
    float radiusFactor = 1.0;
    if (scaleByRadius > 0.0) {
        float ratio = radius > 0.0 ? radius / refRadius : 1.0;
        radiusFactor = max(1.0 + scaleByRadius * (ratio - 1.0), 0.05);
    }
    float scale = baseScale * (1.0 + jitter) * radiusFactor;
    #undef DRAW
    if (scale < 1e-6) return false;

    // Leaf frame columns: X side, Y normal, Z forward, each scaled.
    R = mat3(sideAxis * scale, N * scale, F * scale);
    return true;
}

// Clump mode: leaf `leaf` of clump `clump`, as bromesh::leafInClump places it
// (bromesh/procedural/leaf_clumps.h), step for step on the same hash.
// Records: (centre.xyz, radius), (twig dir.xyz, 0). Header: [0] seed, up
// bias, tilt jitter, shell; [1] base scale, scale jitter.
float procDraw(uint base, uint k) {
    return float(procHash(base + k * 0x9e3779b9u) >> 8) * (1.0 / 16777216.0);
}

// Whether a forest tree (`tree`, its instance) leaves clump `clump` bare:
// each tree drops its own few (up to a fifth, by a hash of the tree), so
// trees of one specimen part their crowns differently. Every tier asks the
// same (the leaves, their casters, the lobes, the crown's trace), so a tree
// keeps its gaps from near to far.
bool procClumpBare(int tree, int clump) {
    uint h = procHash(uint(tree) * 0x9e3779b9u ^ 0x7f4a7c15u);
    float share = 0.2 * float(h >> 8) * (1.0 / 16777216.0);
    return float(procHash(h ^ (uint(clump) * 0x632be5abu)) >> 8) * (1.0 / 16777216.0) < share;
}

void procClumpLobe(vec4 c, int clump, int lobe, uint seed, out vec3 center, out float radius) {
    if (lobe <= 0) {
        center = c.xyz;
        radius = c.w * 0.72;
        return;
    }
    uint cb = procHash(seed ^ procHash(uint(clump) * 0x632be5abu + 0x51ed27u));
    uint b = procHash(cb + uint(lobe) * 0x9e3779b9u);
    float z = procDraw(b, 0u) * 1.2 - 0.4;
    float phi = procDraw(b, 1u) * PROC_TWO_PI;
    float s = sqrt(max(0.0, 1.0 - z * z));
    center = c.xyz + vec3(s * cos(phi), z, s * sin(phi)) * (c.w * 0.42);
    radius = c.w * (0.5 + 0.2 * procDraw(b, 2u));
}

// The core of it, for clump record `c` (centre.xyz, radius): leaf `leaf` (its
// placement key) of clump `clump`, and the lobe it sits in. The header's [0]
// and [1] are the scatter's (the forest node keeps the same two).
bool clumpLeafFrame(vec4 c, int leaf, int clump, out vec3 P, out mat3 R, out vec3 F, out vec3 lc, out float lr) {
    vec4 h0 = procedural.header[0];
    vec4 h1 = procedural.header[1];
    uint seed = floatBitsToUint(h0.x);
    float upBias = h0.y, tiltJitter = h0.z, shell = h0.w;

    uint base = procHash(seed ^ procHash(uint(leaf) + 1u));
    int lobe = min(2, int(procDraw(base, 0u) * 3.0));
    procClumpLobe(c, clump, lobe, seed, lc, lr);

    float z = procDraw(base, 1u) * 2.0 - 1.0;
    float phi = procDraw(base, 2u) * PROC_TWO_PI;
    float s = sqrt(max(0.0, 1.0 - z * z));
    vec3 u = vec3(s * cos(phi), z, s * sin(phi));
    vec3 off = u * (lr * (1.0 - shell * procDraw(base, 3u)));
    off.y *= u.y < 0.0 ? 0.7 : 0.9;
    P = lc + off;

    vec3 N = normalize(u * (1.0 - upBias) + vec3(0.0, upBias, 0.0));
    vec3 jit = vec3(procDraw(base, 4u), procDraw(base, 5u), procDraw(base, 6u)) * 2.0 - 1.0;
    N = N + jit * tiltJitter;
    N = dot(N, N) < 1e-8 ? u : normalize(N);

    float theta = procDraw(base, 7u) * PROC_TWO_PI;
    vec3 t0 = perpendicularUnit(N);
    vec3 t1 = cross(N, t0);
    F = normalize(t0 * cos(theta) + t1 * sin(theta));
    vec3 X = cross(F, N);

    float scale = h1.x * (1.0 + (procDraw(base, 8u) * 2.0 - 1.0) * h1.y);
    R = mat3(1.0);
    if (scale < 1e-6) return false;
    R = mat3(X * scale, N * scale, F * scale);
    return true;
}

bool scatterClumpLeaf(int leaf, int clump, out vec3 P, out mat3 R, out vec3 F) {
    vec3 lc;
    float lr;
    return clumpLeafFrame(procedural.records[clump * 2], leaf, clump, P, R, F, lc, lr);
}

// Whether the draw carries vegetation records (scene_vegetation.glsl).
bool proceduralHasVegetation() { return procedural.header[2].z > 0.5; }

// Segment `seg`'s vegetation record at `along` (0 at from, 1 at to) for a
// point `anchor` on it (node space). v3.x is left zero: a leaf passes its
// stem axis to vegetationSwayAxis itself.
void proceduralVegetation(int seg, float along, vec3 anchor,
                          out vec4 v0, out vec4 v1, out vec4 v2, out vec4 v3) {
    int b = int(procedural.header[2].z + 0.5) + seg * 4;
    vec4 r2 = procedural.records[b + 2];
    v0 = procedural.records[b];
    v1 = procedural.records[b + 1];
    v2 = vec4(anchor, mix(r2.x, r2.y, along));
    v3 = procedural.records[b + 3];
}
