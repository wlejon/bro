// A forest bark vertex (scene_forest.glsl's segment records): segment
// `first + vertex / (sides * 6)` of the entry, `sides` quads around as two
// triangles across its rings, each end ring in the plane across the bark's
// direction there (tubeRingFrame), as scene_procedural.glsl's tubeVertex
// builds a branch tube. False past the entry's run or for a degenerate
// segment.
bool forestTubeVertex(ForestEntry e, int vertexIndex, out vec3 pos, out vec3 radial, out vec3 axisDir, out vec2 uv,
                      out int seg, out float endT) {
    int sides = max(e.param, 3);
    int perSeg = sides * 6;
    int k = vertexIndex / perSeg;
    int local = vertexIndex - k * perSeg;
    int side = local / 6;
    int corner = local - side * 6;
    seg = e.first + k;
    pos = vec3(0.0);
    radial = vec3(0.0);
    axisDir = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    endT = 0.0;
    if (seg >= e.end) return false;

    // corner 0=b0 1=b1 2=t0   3=t0 4=b1 5=t1 (b = from ring, t = to ring)
    bool top = corner == 2 || corner == 3 || corner == 5;
    bool nextSide = corner == 1 || corner == 4 || corner == 5;
    float ang = float(nextSide ? side + 1 : side) / float(sides) * PROC_TWO_PI;
    endT = top ? 1.0 : 0.0;

    int r = forestSegmentRecord(seg);
    vec4 s0 = procedural.records[r];
    vec4 s1 = procedural.records[r + 1];
    vec4 j = procedural.records[r + 2];
    vec3 axis = s1.xyz - s0.xyz;
    float len = length(axis);
    if (len < 1e-6) return false;
    axisDir = axis / len;
    vec3 T = procOctDecode(top ? j.zw : j.xy);
    if (dot(axisDir, T) > 0.2) axisDir = T;
    vec3 e1, e2;
    tubeRingFrame(axisDir, e1, e2);
    radial = cos(ang) * e1 + sin(ang) * e2;
    float rad = mix(s0.w, s1.w, endT);
    pos = (top ? s1.xyz : s0.xyz) + rad * radial;
    uv = vec2(ang / PROC_TWO_PI, endT);
    return true;
}
