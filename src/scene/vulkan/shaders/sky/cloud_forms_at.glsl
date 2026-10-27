// The forms at a point of the deck as the march draws them (sky/clouds.frag),
// shared with the volume's bakes (sky/clouds_volume*.comp), which bake the
// same at a cell's footprint: the point as the cache sees it, the cache read
// over the two levels about a footprint, and the forms there.
//
// The includer has included sky/scene_cloud_layer.glsl.

// An air-space point as the cache sees it: the body's point, on the chart,
// its height in the deck.
struct ClAt {
    vec3 p;
    vec2 xy;
    float h;
};
ClAt clAt(vec3 a) {
    float r = length(a);
    vec3 p = cloudToBody(a);
    return ClAt(p, cloudChartAt(p / r), (r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x));
}
// The same for a point `p` of the body's frame.
ClAt clAtBody(vec3 p) {
    float r = length(p);
    return ClAt(p, cloudChartAt(p / r), (r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x));
}

// The cache's level about footprint `foot` at `w`, as a fraction: its
// whole part the finer of the two read (whose window holds the point), the
// rest how far toward the coarser.
float clLod(ClAt w, float foot) { return cloudCacheLod(w.xy, foot); }

// The cache at `w`, blended over the two levels about `lod` (none past the
// coarsest).
vec4 clCache(ClAt w, float lod) {
    int k = int(lod);
    if (k >= CL_CACHE_LEVELS) return vec4(0.0, 0.0, 0.0, 1e9);
    vec4 a = textureLod(cloudCacheMap, cloudCacheCoord(w.xy, w.h, k), 0.0);
    float f = fract(lod);
    if (k + 1 >= CL_CACHE_LEVELS || f <= 0.0) return a;
    return mix(a, textureLod(cloudCacheMap, cloudCacheCoord(w.xy, w.h, k + 1), 0.0), f);
}
// The cache's slabs' base (x), how much of the skin's move shows over them
// (y) and its heaps' base (z: the world's share of them where its cells are
// as wide as they are), from the cache's extinction's image (what every
// sample reads; the depths only what is lit does).
vec3 clCacheBase(ClAt w, float lod) {
    int k = int(lod);
    if (k >= CL_CACHE_LEVELS) return vec3(0.0, 1.0, 0.0);
    vec3 a = textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k), 0.0).gba;
    float f = fract(lod);
    if (k + 1 >= CL_CACHE_LEVELS || f <= 0.0) return a;
    return mix(a, textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k + 1), 0.0).gba, f);
}

// The base the march reads at `w` (`pd` drifted) over footprint `foot`
// (x), and how much of the skin's move shows (y): the most of the slabs'
// (the cache's) and the heaps' (the heaps' map's columns, sky/
// clouds_layout.h: the lattice's heaps as themselves at every distance,
// toward their share in the cache where they are finer than the pixel);
// the skin shows as the cache says.
// Its parts: x the slabs', y the heaps' (-1 with no map), z how much of the
// skin's move shows, w how much of the heaps' is their share (cloudHeapsFar):
// the heaps' own field changes no faster than a metre's distance a metre,
// where the slabs' may rise CLOUD_EDGE_STEEPEST times that (clouds.frag's clBaseClearParts).
vec4 clBaseParts(ClAt w, float lod, vec3 pd, float foot) {
    vec3 c = clCacheBase(w, lod);
    if (cloudHeapMapSide() <= 0.0) return vec4(c.x, -1.0, c.y, 1.0);
    float texel;
    float in_ = cloudHeapMapIn(cloudChartAt(normalize(pd)), foot, w.h * (clouds.shell.y - clouds.shell.x), texel);
    float far = cloudHeapsFar(foot, texel);
    float own = far < 1.0 ? min(0.5 + in_ / clouds.forms.w, 1.0) : 0.0;
    return vec4(c.x, mix(own, c.z, far), c.y, far);
}
vec2 clBaseShows(ClAt w, float lod, vec3 pd, float foot) {
    vec4 b = clBaseParts(w, lod, pd, foot);
    return vec2(max(b.x, b.y), b.z);
}
float clBase(ClAt w, float lod, float foot) { return clBaseShows(w, lod, cloudDrift(w.p), foot).x; }

// The forms at a point of the view.
struct ClForms {
    float sigma;    // their extinction
    float face;     // their field less its surface's (cloudFieldOf): > 0 in cloud, < 0 out
    float cover;    // how much of the pixel they fill here (its share across the surface's ramp; all of
                    // it where the surface's blur is a cloud's edge or wider, as the share's extinction says)
};

// The forms at `w` (`lod` the cache's level there) over footprint `foot`,
// with the detail or without, as a pixel of footprint `seen` shows them
// (how much of the skin's move shows is the pixel's, not the length a
// piece's mean is taken over: cloudSkinAt).
ClForms clFormsAt(ClAt w, float lod, float foot, float seen, bool detail) {
    ClForms f = ClForms(0.0, -1.0, 0.0);
    if (w.h <= 0.0 || w.h >= 1.0) return f;
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
    vec2 bs = clBaseShows(w, lod, pd, foot);
    float base = max(bs.x, 0.0);
    float wD = detail ? cloudDetailAt(foot) : 0.0;
    vec4 e = vec4(0.0);
    float inCloud = 0.5;
    if (wD > 0.0) {
        // (The column's band at the finer level: what is wisp under a
        // cloud and what cauliflower over it is smooth across levels.)
        vec4 b = cloudCacheBand(w.xy, min(int(lod), CL_CACHE_LEVELS - 1));
        inCloud = clamp((w.h - b.x) / max(b.y - b.x, 1e-3), 0.0, 1.0);
        e = cloudDetail(pd, foot);
    }
    // (The skin's move none under a flat base. The cache's bins are a few
    // hundred metres high, so its steep rise under a base spans the lower
    // walls too; where the detail shows the height in the cloud, the skin
    // moves all of a wall from a little over the base up, which else would
    // be a smooth rounded band under the turrets.)
    float shows = cloudSkinAt(seen);
    float moves = mix(bs.y, max(bs.y, smoothstep(0.04, 0.12, inCloud)), wD);
    float slope;
    float skin = cloudSkinOf(n, e, wD, inCloud, shows * moves, slope);
    float wB = min(cloudBlur(foot) + CLOUD_SKIN_BLUR * (1.0 - shows), 1.0);
    // (The share ramps across the surface over the field's span over the
    // pixel, the turrets' slope with the base's: else where they are steep
    // it ramps in a sliver of a pixel and the silhouette steps.)
    float ramp = wB + cloudSkinBlur(slope, seen);
    f.sigma = cloudSigmaOf(base, skin, wB, foot, ramp);
    float face = cloudFieldOf(base, skin, wB) + 0.5 * wB;
    f.face = f.sigma > 0.0 ? max(face, 1e-5) : min(face, -1e-5);
    f.cover = f.sigma > 0.0 ? mix(clamp(face / ramp, 0.0, 1.0), 1.0, wB) : 0.0;
    return f;
}
