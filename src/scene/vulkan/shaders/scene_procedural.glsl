// The segment records a procedural instanced draw (foliage scatter, branch
// tubes) builds its geometry from: set 3 binding 1, the slot skinned meshes
// use binding 0 of for their bone palette. SceneMeshDrawer::prepare writes it.
//
//   header[0..2]   the mode's parameters (see each shader); header[2].z is the
//                  first vegetation record (0: the node carries none)
//   records[]      2 per segment, then (scatter) the per-leaf segment indices
//                  packed four to a record, then (with vegetation) 3 per
//                  segment: (root.xyz, height), (crown.xyz, crownRadius),
//                  (weightFrom, weightTo, phase, 0) — scene_vegetation.glsl

layout(std430, set = 3, binding = 1) readonly buffer ProceduralSegments {
    vec4 header[3];
    vec4 records[];
} procedural;

const float PROC_TWO_PI = 6.28318530718;

// Any unit vector perpendicular to unit `t` (bromesh's perpendicularUnit), so
// tube rings and leaf frames share a basis.
vec3 perpendicularUnit(vec3 t) {
    vec3 c = cross(t, vec3(0.0, 1.0, 0.0));
    if (dot(c, c) < 1e-8) c = cross(t, vec3(1.0, 0.0, 0.0));
    return normalize(c);
}

// The vertex of a tube draw: segment from the vertex index, `sides` quads a
// segment as two triangles across its rings. Records: (from.xyz, radiusFrom),
// (to.xyz, radiusTo). Header: x sides, y radius scale. `seg` and `endT` (0 at
// from, 1 at to) say where on the skeleton the vertex sits. False for a
// degenerate segment.
bool tubeVertex(int vertexIndex, out vec3 pos, out vec3 radial, out vec3 axisDir, out vec2 uv,
                out int seg, out float endT) {
    int sides = int(procedural.header[0].x);
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

    vec4 s0 = procedural.records[seg * 2];
    vec4 s1 = procedural.records[seg * 2 + 1];
    vec3 axis = s1.xyz - s0.xyz;
    float len = length(axis);
    pos = vec3(0.0);
    radial = vec3(0.0);
    axisDir = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    if (len < 1e-6) return false;
    axisDir = axis / len;
    vec3 e1 = perpendicularUnit(axisDir);
    vec3 e2 = cross(axisDir, e1);
    radial = cos(ang) * e1 + sin(ang) * e2;
    float r = mix(s0.w, s1.w, endT) * procedural.header[0].y;
    pos = mix(s0.xyz, s1.xyz, endT) + r * radial;
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

// Whether the draw carries vegetation records (scene_vegetation.glsl).
bool proceduralHasVegetation() { return procedural.header[2].z > 0.5; }

// Segment `seg`'s vegetation record at `along` (0 at from, 1 at to) for a
// point `anchor` on it, with stem `axis` (zero for wood), all node space.
void proceduralVegetation(int seg, float along, vec3 anchor, vec3 axis,
                          out vec4 v0, out vec4 v1, out vec4 v2, out vec4 v3) {
    int b = int(procedural.header[2].z + 0.5) + seg * 3;
    vec4 r2 = procedural.records[b + 2];
    v0 = procedural.records[b];
    v1 = procedural.records[b + 1];
    v2 = vec4(anchor, mix(r2.x, r2.y, along));
    v3 = vec4(axis, r2.z);
}
