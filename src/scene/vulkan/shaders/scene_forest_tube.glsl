// A forest bark vertex (scene_forest.glsl's segment records): segment
// `first + vertex / (2 (sides + 1))` of the entry, a point of its start or
// end ring (the drawer's index pattern joins them in `sides` quads of two
// triangles), each end ring in the plane across the bark's
// direction there (tubeRingFrame), as scene_procedural.glsl's tubeVertex
// builds a branch tube. False past the entry's run or for a degenerate
// segment.
//
// forestTubeVertexMorph also morphs the ring's sides by its width on screen
// (forestTubeMorph; `model` the tree's, `sidesPx` Lod::barkSidesPixels);
// forestTubeVertex leaves them as they are (the casters).

// Point `j` of a ring of `n` sides as a ring of `m` (m divides n) draws it:
// on the chord of m's sides it falls between, as a unit-circle offset.
vec2 forestTubeLevel(int j, int n, int m) {
    float x = float(j * m) / float(n);
    float k = floor(x);
    float a0 = k / float(m) * PROC_TWO_PI, a1 = (k + 1.0) / float(m) * PROC_TWO_PI;
    return mix(vec2(cos(a0), sin(a0)), vec2(cos(a1), sin(a1)), x - k);
}

// A ring of `n` sides (5, 10 or 20) `widthPx` wide on screen, drawn as 5
// sides under `sidesPx`, morphing to 10 by twice it, and to 20 from 2.5
// times it to twice that: the offset of its point j (the unit-circle
// direction's share, cos/sin). A tree draws 5 sides only while its trunk is
// under `sidesPx` and 10 under Lod::barkFineSides (2.5) times it, so
// whichever it draws, a ring of a given width has the one outline: neither
// the tree's switch nor the narrowing toward its twigs shows a step.
vec2 forestTubeMorph(int j, int n, float widthPx, float sidesPx) {
    vec2 own = vec2(cos(float(j) / float(n) * PROC_TWO_PI), sin(float(j) / float(n) * PROC_TWO_PI));
    if (n < 10) return own;
    vec2 p10 = n == 10 ? own : forestTubeLevel(j, n, 10);
    vec2 r = mix(forestTubeLevel(j, n, 5), p10, smoothstep(sidesPx, 2.0 * sidesPx, widthPx));
    if (n >= 20) r = mix(r, own, smoothstep(2.5 * sidesPx, 5.0 * sidesPx, widthPx));
    return r;
}

bool forestTubeVertexMorph(ForestEntry e, int vertexIndex, mat4 model, float sidesPx, out vec3 pos, out vec3 radial,
                           out vec3 axisDir, out vec2 uv, out int seg, out float endT) {
    // Drawn indexed (the drawer's woodIndices): a segment is two rings of
    // sides + 1 points, its start ring's then its end ring's.
    int sides = max(e.param, 3);
    int ring = sides + 1;
    int k = vertexIndex / (2 * ring);
    int local = vertexIndex - k * 2 * ring;
    bool top = local >= ring;
    int pt = top ? local - ring : local;
    seg = e.first + k;
    pos = vec3(0.0);
    radial = vec3(0.0);
    axisDir = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    endT = 0.0;
    if (seg >= e.end) return false;

    float ang = float(pt) / float(sides) * PROC_TWO_PI;
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
    // The drawer leaves out bark thinner than the entry's radius (its
    // `widening` slot, specimen units): bark under 1.6 times it thins to
    // nothing there, so a twig fades out as the eye backs off rather than
    // blinking out between two frames.
    if (e.widening > 0.0) rad *= smoothstep(e.widening, 1.6 * e.widening, rad);
    vec3 centre = top ? s1.xyz : s0.xyz;
    vec2 at = vec2(cos(ang), sin(ang));
    if (sidesPx > 0.0 && sides >= 10) {
        // Its width on screen at the ring's own distance (as the drawer
        // measures the trunk's at the tree's nearest point).
        float dist = max(length((model * vec4(centre, 1.0)).xyz), 1e-3);
        bool ortho = abs(camera.proj[3][3]) > 0.5;
        float pxPerM = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : dist);
        at = forestTubeMorph(pt, sides, 2.0 * rad * vegScale(model) * pxPerM, sidesPx);
    }
    pos = centre + rad * (at.x * e1 + at.y * e2);
    uv = vec2(ang / PROC_TWO_PI, endT);
    return true;
}

bool forestTubeVertex(ForestEntry e, int vertexIndex, out vec3 pos, out vec3 radial, out vec3 axisDir, out vec2 uv,
                      out int seg, out float endT) {
    return forestTubeVertexMorph(e, vertexIndex, mat4(1.0), 0.0, pos, radial, axisDir, uv, seg, endT);
}
