// The segment records a procedural instanced draw (foliage scatter, branch
// tubes) builds its geometry from: set 3 binding 1, the slot skinned meshes
// use binding 0 of for their bone palette. SceneMeshDrawer::prepare writes it.
//
//   header[0..2]   the mode's parameters (see each shader)
//   records[]      2 per segment, then (scatter) the per-leaf segment indices
//                  packed four to a record

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
// (to.xyz, radiusTo). Header: x sides, y radius scale. False for a degenerate
// segment.
bool tubeVertex(int vertexIndex, out vec3 pos, out vec3 radial, out vec3 axisDir, out vec2 uv) {
    int sides = int(procedural.header[0].x);
    int perSeg = sides * 6;
    int seg = vertexIndex / perSeg;
    int local = vertexIndex - seg * perSeg;
    int side = local / 6;
    int corner = local - side * 6;

    // corner 0=b0 1=b1 2=t0   3=t0 4=b1 5=t1 (b = from ring, t = to ring)
    bool top = corner == 2 || corner == 3 || corner == 5;
    bool nextSide = corner == 1 || corner == 4 || corner == 5;
    float ang = float(nextSide ? side + 1 : side) / float(sides) * PROC_TWO_PI;
    float endT = top ? 1.0 : 0.0;

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
