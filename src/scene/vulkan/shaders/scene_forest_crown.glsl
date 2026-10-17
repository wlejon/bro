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
    int b = int(floatBitsToUint(procedural.header[3].x)) + i * 2;
    vec4 p = procedural.records[b];
    vec4 q = procedural.records[b + 1];
    specimen = int(floatBitsToUint(q.z));
    float s = p.w;
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
};

// One clump on the ray from the eye (the origin) along unit `d`: the entry
// and exit distances of its ellipsoid (t.x > t.y when missed).
vec2 forestCrownChord(vec3 d, vec3 c, float r) {
    // The ellipsoid: radius r across, 0.8 r up, its middle 0.1 r above the
    // clump's centre (0.9 up, 0.7 down). Scaled to a sphere.
    vec3 o = -(c + vec3(0.0, 0.1 * r, 0.0));
    o.y *= 1.25;
    vec3 e = vec3(d.x, d.y * 1.25, d.z);
    float a = dot(e, e), b = dot(o, e), k = dot(o, o) - r * r;
    float disc = b * b - a * k;
    if (disc <= 0.0) return vec2(1.0, 0.0);
    float s = sqrt(disc);
    return vec2((-b - s) / a, (-b + s) / a);
}

// The crown of instance `instance` along unit `d`, its clumps moved by
// `offset` (the crown's pose). `len` and `aspect` a leaf's length (world)
// and width over length; `grain` the pixel's wander (forestMassGrain).
ForestCrownHit forestCrownTrace(vec3 d, int instance, vec3 offset, float len, float aspect, vec4 grain) {
    ForestCrownHit h;
    h.cover = 0.0;
    h.at = vec3(0.0);
    h.normal = -d;
    h.sphere = vec4(0.0, 0.0, 0.0, 1.0);
    h.density = 0.0;
    h.near = 1.0;

    int specimen;
    mat4 model = push.model * forestCrownInstance(instance, specimen);
    float scale = length(model[1].xyz);
    int sr = int(floatBitsToUint(procedural.header[2].x)) + specimen * 4;
    uvec4 counts = floatBitsToUint(procedural.records[sr]);
    int first = int(counts.z), count = int(counts.w);
    int clumpBase = int(floatBitsToUint(procedural.header[2].z));
    // A leaf's area facing any way (forestMassDepth's 0.55 of its ellipse).
    float a = 0.5 * len;
    float leafArea = 3.14159265 * a * a * aspect * 0.55;

    // The two nearest clumps the ray enters, and the optical depth of all.
    float t1 = 1e30, t2 = 1e30, tau1 = 0.0, tau2 = 0.0, tauAll = 0.0;
    vec4 s1 = vec4(0.0, 0.0, 0.0, 1.0), s2 = s1;
    float d1 = 0.0, d2 = 0.0;
    for (int i = 0; i < count; ++i) {
        if (procClumpBare(instance, first + i)) continue;
        int r = clumpBase + (first + i) * 6;
        vec4 c = procedural.records[r];
        float leaves = procedural.records[r + 1].w;
        vec3 C = (model * vec4(c.xyz, 1.0)).xyz + offset;
        float R = c.w * scale;
        vec2 t = forestCrownChord(d, C, R);
        if (t.x >= t.y || t.y <= 0.0) continue;
        // Its leaves over its ellipsoid's volume (4/3 pi r^3 0.8).
        float sigma = leaves * leafArea / (3.351 * R * R * R);
        float tau = sigma * (t.y - max(t.x, 0.0)) * max(1.0 + 1.7 * grain.w, 0.0);
        tauAll += tau;
        if (t.x < t1) {
            t2 = t1; tau2 = tau1; s2 = s1; d2 = d1;
            t1 = t.x; tau1 = tau; s1 = vec4(C, R); d1 = sigma;
        } else if (t.x < t2) {
            t2 = t.x; tau2 = tau; s2 = vec4(C, R); d2 = sigma;
        }
    }
    if (tauAll <= 0.0) return h;
    h.cover = 1.0 - exp(-tauAll);
    // The nearest clump's share of what is seen, the second's through it.
    float w1 = 1.0 - exp(-tau1), w2 = exp(-tau1) * (1.0 - exp(-tau2));
    float wSum = max(w1 + w2, 1e-6);
    h.near = w1 / wSum;
    bool first1 = h.near >= 0.5;
    float tSeen = first1 ? t1 : t2;
    vec4 S = first1 ? s1 : s2;
    float sig = first1 ? d1 : d2;
    // The leaf seen lies about a free path in.
    float tIn = max(tSeen, 0.0) + min(1.0 / max(sig, 1e-4), S.w);
    h.at = d * tIn;
    vec3 rel = h.at - (S.xyz + vec3(0.0, 0.1 * S.w, 0.0));
    rel.y *= 1.5625;   // the ellipsoid's normal
    h.normal = normalize(rel);
    h.sphere = S;
    h.density = sig;
    return h;
}
