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
// The base (x) and how much of the skin's move shows (y), from the cache's
// extinction's image (what every sample reads; the depths only what is lit
// does).
vec2 clBaseShows(ClAt w, float lod) {
    int k = int(lod);
    if (k >= CL_CACHE_LEVELS) return vec2(0.0, 1.0);
    vec2 a = textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k), 0.0).gb;
    float f = fract(lod);
    if (k + 1 >= CL_CACHE_LEVELS || f <= 0.0) return a;
    return mix(a, textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k + 1), 0.0).gb, f);
}
float clBase(ClAt w, float lod) { return clBaseShows(w, lod).x; }

// How much of the lumps a share is carved into this view shows
// (cloudLumpsOf): all from over the deck, none along a view that meets it
// at a slope under CL_LUMPS_FROM (its sine; the view's where it enters the
// deck), so flat that a field's heaps stand one behind another and their
// gaps close (and a view through it would find each heap's faces in turn).
const float CL_LUMPS_FROM = 0.08, CL_LUMPS_TO = 0.2;
float clLumps = 1.0;

// How much the cache's base at level `lod` is a share cut into clouds
// (cloudCutBase), not the world's surface.
float clShareLike(float lod) { return cloudShareLike(CL_CACHE_TEXEL0 * exp2(lod)); }
// How much the view looks down into the deck (the sine of its slope below
// the horizontal where it enters it, CL_DOWN_FROM .. TO): a cloud seen from
// above, its sunlit top, the eye over the deck (all of it CL_DOWN_OVER of
// the deck's thickness over its top). There a share cut into clouds is
// opaque at heart (cloudCutSteep), and the deep body's diffuse light
// reaches the eye (clIntegrate); a view from below, across or within the
// deck, at a base or a side, is as it was.
const float CL_DOWN_FROM = 0.05, CL_DOWN_TO = 0.2;
const float CL_DOWN_OVER = 0.25;
float clDown = 0.0;

// The forms at a point of the view.
struct ClForms {
    float sigma;    // their extinction
    float face;     // their field less its surface's (cloudFieldOf): > 0 in cloud, < 0 out
};

// The forms at `w` (`lod` the cache's level there) over footprint `foot`,
// with the detail or without, as a pixel of footprint `seen` shows them
// (how much of the skin's move shows is the pixel's, not the length a
// piece's mean is taken over: cloudSkinAt).
ClForms clFormsAt(ClAt w, float lod, float foot, float seen, bool detail) {
    ClForms f = ClForms(0.0, -1.0);
    if (w.h <= 0.0 || w.h >= 1.0) return f;
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
    vec2 bs = clBaseShows(w, lod);
    float base = max(bs.x + cloudLumpsOf(pd, foot, clShareLike(lod) * clLumps), 0.0);
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
    f.sigma = cloudSigmaOf(base, skin, wB, foot, ramp, cloudCutSteep(clShareLike(lod) * clDown));
    float face = cloudFieldOf(base, skin, wB) + 0.5 * wB;
    f.face = f.sigma > 0.0 ? max(face, 1e-5) : min(face, -1e-5);
    return f;
}
