// A forest CROWN (mesh_forest_sprite.vert, mode 1): one card a tree, whose
// pixels find the tree's own clumps along their rays, so a far crown keeps
// the clumps tier's masses, gaps, outline and true extent rather than
// standing in for them with one sphere. Every clump is the ellipsoid its
// leaves fill (bromesh leafInClump: lobes round the clump's centre, the
// shell flattened underneath, 0.9 of the radius up and 0.7 down), holding
// its own leaf count; a ray through it is covered as its leaves would cover
// it (their optical depth along the chord, scene_forest_mass.glsl's
// forestMassDepth per unit length). The nearest two clumps a ray meets are
// the ones seen: the first, and the second through the first's gaps. Nothing
// here is baked: the clumps are the specimen's records, posed rigidly with
// the crown (the card's pose), so any tree of any specimen draws this way.
// Needs scene_procedural.glsl (the forest's records) and
// scene_forest_mass.glsl.

// The instance's transform, specimen space to camera-relative node space
// (scene_forest.glsl's forestInstance, without the entries it needs).
mat4 forestCrownInstance(int i, out int specimen) {
    int b = FOREST_BASE(FOREST_H_INSTANCES) + i * FOREST_INSTANCE_RECORDS;
    vec4 p = FOREST_RECORD(b, FOREST_INST_POSITION);
    vec4 q = FOREST_RECORD(b, FOREST_INST_COS_YAW);
    specimen = int(floatBitsToUint(q[FOREST_INST_SPECIMEN % 4]));
    float s = p[FOREST_INST_SCALE % 4];
    return mat4(vec4(q.x * s, 0.0, -q.y * s, 0.0), vec4(0.0, s, 0.0, 0.0), vec4(q.y * s, 0.0, q.x * s, 0.0),
                vec4(p.xyz, 1.0));
}

struct ForestCrownHit {
    float cover;     // the share of the pixel the clumps' leaves cover
    vec3 at;         // where the leaves seen lie (camera-relative)
    vec3 normal;     // their bulk facing: out of the clump they lie in (unit)
    vec4 sphere;     // that clump: centre (camera-relative), radius
    float density;   // its leaves' optical depth a unit length
    float near;      // the share of the cover the nearest clump gives (the rest: the second's)
    float flatness;  // that clump's height over its width (SceneSurface::leafFlat)
    // The whole crown's leaves over its ellipsoid's volume (its records'
    // crown radius across and up): their optical depth a unit length, which
    // shades the leaves seen by the crown's other clumps between them and
    // the sun, as the clumps tier's lobes are by each other's casters.
    float crownSigma;
};

// A broadleaf clump's leaves fill its lobes (bromesh leafClumpLobe), which
// leave its ellipsoid's rim open: traced as the ellipsoid, as many leaves in
// this fraction of its radius (at the full radius its far crowns covered a
// fifth more than its leaves).
const float FOREST_CROWN_FILL = 0.8;

// The share of a broadleaf crown's mean density (ForestCrownHit::crownSigma)
// its leaves are shaded through: as a lobe takes half its own.
const float FOREST_CROWN_SHADE = 0.5;
// The most of that density the light crosses to the leaves seen, which lie
// within about one optical depth of the crown's surface along the eye's line
// (what hides the rest), where the path from the point the trace stops at
// runs on through the crown. A dense shrub's crown was shaded through its
// whole depth: 30 levels darker than its leaves, near black from afar.
const float FOREST_CROWN_SELF_MAX = 0.75;

// A crown is traced a run of clumps at a time (coarse) once its mean clump
// is under this many pixels in radius.
const float FOREST_COARSE_PIXELS = 3.0;

// The crown of instance `instance` along unit `d`, its clumps moved by
// `offset` (the crown's pose). `len` and `aspect` a leaf's length (world)
// and width over length; `grain` the pixel's wander (forestMassGrain).
// `pixel` the size of a pixel (world) at the crown: where its clumps are
// each under about a pixel, each run of them is traced as the one
// ellipsoid it fills (the block's second and third records), not clump by
// clump, the crown's outline and mass the same at a fraction of the cost.
// `pick` the pixel's own draw in [0, 1).
ForestCrownHit forestCrownTrace(vec3 d, int instance, vec3 offset, float len, float aspect, vec4 grain,
                                float pixel, float pick) {
    ForestCrownHit h;
    h.crownSigma = 0.0;
    h.cover = 0.0;
    h.at = vec3(0.0);
    h.normal = -d;
    h.sphere = vec4(0.0, 0.0, 0.0, 1.0);
    h.density = 0.0;
    h.near = 1.0;
    h.flatness = 1.0;

    int specimen;
    mat4 model = push.model * forestCrownInstance(instance, specimen);
    float scale = length(model[1].xyz);
    int sr = FOREST_BASE(FOREST_H_SPECIMENS) + specimen * FOREST_SPECIMEN_RECORDS;
    uvec4 counts = floatBitsToUint(FOREST_RECORD(sr, FOREST_SP_SEG_START));
    int first = int(counts[FOREST_SP_CLUMP_START % 4]), count = int(counts[FOREST_SP_CLUMP_COUNT % 4]);
    int clumpBase = FOREST_BASE(FOREST_H_CLUMPS);
    // A leaf's area facing any way (forestMassDepth's 0.55 of its ellipse).
    float a = 0.5 * len;
    float leafArea = 3.14159265 * a * a * aspect * 0.55;
    // Its clumps' height over their width (scene_forest.glsl's forestClumpFlat).
    float flatness = FOREST_FLOAT(sr, FOREST_SP_CLUMP_FLAT) > 0.0 ? FOREST_FLOAT(sr, FOREST_SP_CLUMP_FLAT) : 1.0;
    // (A spray node's clumps are its sprays' pads, filled to their rims; a
    // needle spray's pad, its needles radiating, leaves its outer tenth
    // open: filled to the rim, a pine's crowns covered 0.6 to 0.9 points
    // more of the view than its leaves at 100 to 150 m.)
    uint form = floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u;
    float fill = form == 1u ? 0.9 : form != 0u ? 1.0 : FOREST_CROWN_FILL;
    // A pinnate specimen's clumps are runs of leaflets along their wood
    // (scene_forest_shoot.glsl's forestPinnateLeaf, its FOREST_SP_PHYLLOTAXIS
    // 3): each traced as the spheroid the clumps tier's run of
    // three lobes fills (scene_forest_sprite_vertex.glsl), its axis the
    // wood's, not the round clump's ellipsoid, which stood out past a
    // hanging whip's narrow curtain either side.
    bool pinnate = int(FOREST_FLOAT(sr, FOREST_SP_PHYLLOTAXIS) + 0.5) == 3;
    {
        float leaves = float(floatBitsToUint(FOREST_FLOAT(sr, FOREST_SP_LEAF_COUNT)));
        float cr = max(FOREST_FLOAT(sr, FOREST_SP_CROWN_RADIUS), 1e-3) * scale;
        float cu = max(FOREST_FLOAT(sr, FOREST_SP_CROWN_UP), 1e-3) * scale;
        h.crownSigma = leaves * leafArea / (4.18879 * cr * cr * cu);
    }

    // The two nearest clumps the ray enters (each its centre, radius across
    // and up), and the optical depth of all.
    float t1 = 1e30, t2 = 1e30, tau1 = 0.0, tau2 = 0.0, tauAll = 0.0;
    vec4 s1 = vec4(0.0, 0.0, 0.0, 1.0), s2 = s1;
    float d1 = 0.0, d2 = 0.0, y1 = 1.0, y2 = 1.0;
    // The leaves' count along the ray wanders pixel to pixel; the grain
    // fades as a pixel takes in more leaves, but a clump's leaves lie in its
    // lobes' shells, not evenly through it, so the count still wanders at
    // the lobes' scale however far off: the pixel's own draw keeps it (with
    // the crown even, its far pixels were all but solid, 10 units brighter
    // than its leaves, whose gaps let the ground through).
    float u = fract(pick * 7.13 + 0.31) * 2.0 - 1.0;
    float wander = max(1.0 + 1.7 * mix(grain.w, u, clamp(1.0 - length(grain) / 0.9, 0.0, 1.0)), 0.0);
    // Density follows what a pixel can show: once a mean clump is a few
    // pixels across or less, every second clump (then every fourth) stands
    // for its run's neighbours, as many leaves in a ball of as much volume
    // (radius times the cube root of the stride), so the crown keeps its
    // outline and mass at a half or a quarter of the chords. The steps are
    // dithered by the pixel's wander so a switch never draws a line.
    float px = FOREST_FLOAT(sr, FOREST_SP_CLUMP_RADIUS) * scale / max(pixel, 1e-6) * (0.8 + 0.4 * grain.z);
    int stride = px < FOREST_COARSE_PIXELS * 0.5 ? 4 : px < FOREST_COARSE_PIXELS ? 2 : 1;
    // (Not a pinnate specimen's: its runs are narrow, and a run's
    // neighbours in its block hang beside it across the curtain, not
    // around it, so no grown run stands in for them; every second one
    // opened the curtain at range, 150 m cover 0.80 of the leaves', not
    // 0.86.)
    if (pinnate) stride = 1;
    float grow = stride == 4 ? 1.587 : stride == 2 ? 1.26 : 1.0;
    // The clumps in runs of FOREST_CLUMP_BLOCK that lie together
    // (ForestNode::kClumpBlock), two records a run from the specimen's first
    // (FOREST_SP_BLOCKS): its bounding sphere, which a ray that misses
    // skips the run by, and its clumps.
    int blocks = floatBitsToInt(FOREST_FLOAT(sr, FOREST_SP_BLOCKS));
    // The ray in the specimen's own space (the instance is a turn about +Y
    // and a uniform scale, so its clumps stay upright there): o + e t, t
    // still the distance from the eye. The runs and clumps are tested where
    // their records are, no transform each.
    mat3 At = transpose(mat3(model));
    float inv2 = 1.0 / max(scale * scale, 1e-12);
    vec3 o = At * (-(model[3].xyz + offset)) * inv2;
    vec3 e = At * d * inv2;
    float ee = dot(e, e);
    for (int b = 0; b * FOREST_CLUMP_BLOCK < count; ++b) {
        vec4 bs = FOREST_RECORD(blocks + b * FOREST_BLOCK_RECORDS, FOREST_BLOCK_SPHERE);
        vec3 v = bs.xyz - o;
        // The grown balls reach a little past the run's sphere.
        float br = bs.w * grow, BR = br * scale;
        float tc = dot(v, e) / ee;
        if (dot(v, v) - tc * tc * ee >= br * br || tc + BR <= 0.0) continue;
        // Once the pixel is covered and its two nearest clumps found, a run
        // wholly behind the second changes neither.
        if (tauAll > 6.0 && tc - BR > t2) continue;
        // The run's clumps, two 16-bit specimen indices to a float.
        uvec4 run = floatBitsToUint(FOREST_RECORD(blocks + b * FOREST_BLOCK_RECORDS, FOREST_BLOCK_CLUMPS));
        for (int j = 0; j < FOREST_CLUMP_BLOCK; j += stride) {
            int i = int((run[j >> 1] >> uint((j & 1) * 16)) & 0xffffu);
            if (i == 0xffff) break;
            if (procClumpBare(instance, first + i)) continue;
            int r = clumpBase + (first + i) * FOREST_CLUMP_RECORDS;
            vec4 c = FOREST_RECORD(r, FOREST_CLUMP_CENTRE);
            float leaves = FOREST_FLOAT(r, FOREST_CLUMP_LEAVES) * float(stride);
            // 0.9 of the radius up and 0.7 down about the clump's centre:
            // 0.8 up about a point 0.1 above it, times the flatness.
            float rr = c.w * grow * fill, ry = 0.8 * flatness * rr;
            vec3 cs = c.xyz + vec3(0.0, 0.1 * rr * flatness, 0.0);
            // The chord, the ellipsoid scaled to a sphere.
            vec3 oc, es;
            if (pinnate) {
                // The run: half its length (the clump's radius) along its
                // wood `ax` about its middle, and a third of that or a
                // leaflet's reach (0.8 of a leaf) across, as the clumps
                // tier's lobes (the same volume as the three of them).
                vec3 D = FOREST_RECORD(r, FOREST_CLUMP_TWIG).xyz;
                vec3 ax = dot(D, D) > 1e-12 ? normalize(D) : vec3(0.0, 1.0, 0.0);
                cs = c.xyz - ax * (0.35 * c.w);
                ry = c.w;
                rr = max(c.w / 3.0, 0.8 * FOREST_HEADER(FOREST_H_LEAF_LENGTH));
                float sa = rr / ry;
                oc = o - cs;
                oc += ax * ((sa - 1.0) * dot(oc, ax));
                es = e + ax * ((sa - 1.0) * dot(e, ax));
            } else {
                float sy = rr / ry;
                oc = o - cs;
                oc.y *= sy;
                es = vec3(e.x, e.y * sy, e.z);
            }
            float qa = dot(es, es), qb = dot(oc, es), qk = dot(oc, oc) - rr * rr;
            float disc = qb * qb - qa * qk;
            if (disc <= 0.0) continue;
            float sq = sqrt(disc);
            vec2 t = vec2(-qb - sq, -qb + sq) / qa;
            if (t.y <= 0.0) continue;
            float R = rr * scale, RY = ry * scale;
            vec3 C = cs;   // (specimen space: the seen one is placed below)
            // Its leaves over its ellipsoid's volume (4/3 pi r^2 ry).
            float sigma = leaves * leafArea / (4.18879 * R * R * RY);
            float tau = sigma * (t.y - max(t.x, 0.0)) * wander;
            tauAll += tau;
            // (Its height over its width; a pinnate run's long axis is its
            // wood's, lit as round.)
            float ky = pinnate ? 1.0 : RY / R;
            if (t.x < t1) {
                t2 = t1; tau2 = tau1; s2 = s1; d2 = d1; y2 = y1;
                t1 = t.x; tau1 = tau; s1 = vec4(C, R); d1 = sigma; y1 = ky;
            } else if (t.x < t2) {
                t2 = t.x; tau2 = tau; s2 = vec4(C, R); d2 = sigma; y2 = ky;
            }
        }
    }
    if (tauAll <= 0.0) return h;
    h.cover = 1.0 - exp(-tauAll);
    // The nearest clump's share of what is seen, the second's through it.
    float w1 = 1.0 - exp(-tau1), w2 = exp(-tau1) * (1.0 - exp(-tau2));
    float wSum = max(w1 + w2, 1e-6);
    h.near = w1 / wSum;
    // Which is seen is drawn by its share (`pick`, the pixel's own draw in
    // [0, 1)), so the second, deeper and darker, shows in its share of the
    // pixels however far off, as the leaves behind the first's gaps do.
    bool first1 = pick < h.near;
    float tSeen = first1 ? t1 : t2;
    vec4 S = first1 ? s1 : s2;
    S.xyz = (model * vec4(S.xyz, 1.0)).xyz + offset;   // to camera-relative, R already world
    float sig = first1 ? d1 : d2;
    float ky = first1 ? y1 : y2;
    // The leaf seen lies about a free path in.
    float tIn = max(tSeen, 0.0) + min(1.0 / max(sig, 1e-4), S.w);
    h.at = d * tIn;
    vec3 rel = h.at - S.xyz;
    rel.y /= ky * ky;   // the ellipsoid's normal
    h.normal = normalize(rel);
    // Its leaves' shade is a clump's own: a far pixel's ball is its run's
    // clumps merged (radius times the grow, at their density), but the leaf
    // seen lies in one of them. (Through the merged ball a cypress's crowns
    // darkened with distance, 86 to 77 from 45 to 120 m at golden, where
    // its leaves brighten.)
    h.sphere = vec4(S.xyz, S.w / grow);
    h.density = sig;
    h.flatness = ky;
    return h;
}
