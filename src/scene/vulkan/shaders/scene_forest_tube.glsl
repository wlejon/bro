// A forest bark vertex (scene_forest.glsl's segment records): segment
// `first + vertex / ((rows + 1) (sides + 1))` of the entry, a point of one
// of its rings from its start to its end (the drawer's index pattern joins
// each two in `sides` quads of two triangles), each ring on the segment's
// curve (forestHermite) in the plane across the bark's way there
// (tubeRingFrame), as scene_procedural.glsl's tubeVertex builds a branch
// tube. False past the entry's run or for a degenerate segment.
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
                           out vec3 axisDir, out vec2 uv, out int seg, out float endT, out float rad) {
    // Drawn indexed (the drawer's woodIndices): a segment is rows + 1 rings
    // of sides + 1 points, from its start ring to its end ring.
    // The entry's param is its sides + 256 x its rows (the drawer's
    // barkRows): the rings past the first, its curve (forestHermite through
    // its two ends along the bark's way there) in rows - 1 bends for the
    // near trees' thick wood, one straight row for the rest.
    int sides = max(e.param & 255, 3);
    int ring = sides + 1;
    int rows = max(e.param >> 8, 1);
    int k = vertexIndex / ((rows + 1) * ring);
    int local = vertexIndex - k * (rows + 1) * ring;
    int row = local / ring;
    int pt = local - row * ring;
    seg = e.first + k;
    pos = vec3(0.0);
    radial = vec3(0.0);
    axisDir = vec3(0.0, 1.0, 0.0);
    uv = vec2(0.0);
    endT = 0.0;
    rad = 0.0;
    if (seg >= e.end) return false;

    float ang = float(pt) / float(sides) * PROC_TWO_PI;
    endT = float(row) / float(rows);

    int r = forestSegmentRecord(seg);
    vec4 s0 = FOREST_RECORD(r, FOREST_SEG_FROM);
    vec4 s1 = FOREST_RECORD(r, FOREST_SEG_TO);
    vec4 j = FOREST_RECORD(r, FOREST_SEG_TANGENTS);
    vec3 axis = s1.xyz - s0.xyz;
    float len = length(axis);
    if (len < 1e-6) return false;
    vec3 chord = axis / len;
    // Its way at each end (a joint's, shared with the segment meeting it
    // there), or the chord's where that is turned far from it.
    vec3 T0 = procOctDecode(j.xy), T1 = procOctDecode(j.zw);
    if (dot(chord, T0) <= 0.2) T0 = chord;
    if (dot(chord, T1) <= 0.2) T1 = chord;
    vec3 centre = forestHermite(s0.xyz, T0, s1.xyz, T1, endT, axisDir);
    if (row == 0) { centre = s0.xyz; axisDir = T0; }
    if (row == rows) { centre = s1.xyz; axisDir = T1; }
    vec3 e1, e2;
    tubeRingFrame(axisDir, e1, e2);
    radial = cos(ang) * e1 + sin(ang) * e2;
    // A tip's end radius is negative (ForestNode::gpuData): nothing goes on
    // from it, so it closes over its last stretch to a rounded point, not
    // an open cut.
    rad = mix(s0.w, abs(s1.w), endT);
    if (s1.w < 0.0) rad *= sqrt(max(1.0 - endT * endT * endT * endT, 0.0));
    // The drawer leaves out bark thinner than the entry's radius (its
    // `widening` slot, specimen units): bark under 1.6 times it thins to
    // nothing there, so a twig fades out as the eye backs off rather than
    // blinking out between two frames.
    if (e.widening > 0.0) rad *= smoothstep(e.widening, 1.6 * e.widening, rad);
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
    float rad;
    return forestTubeVertexMorph(e, vertexIndex, mat4(1.0), 0.0, pos, radial, axisDir, uv, seg, endT, rad);
}
