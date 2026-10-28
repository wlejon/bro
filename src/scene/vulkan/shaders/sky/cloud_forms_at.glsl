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
// The cache's extinction at `w` (the forms' mean over a cell, what the
// light and the shadow read), blended over the two levels about `lod`.
float clCacheSigma(ClAt w, float lod) {
    int k = int(lod);
    if (k >= CL_CACHE_LEVELS) return 0.0;
    float a = textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k), 0.0).r;
    float f = fract(lod);
    if (k + 1 >= CL_CACHE_LEVELS || f <= 0.0) return a;
    return mix(a, textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k + 1), 0.0).r, f);
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
// (Whether the heaps are themselves or their share is the pixel's, `seen`,
// not the length a mean is taken over: a piece of the sun's ray a few
// hundred metres long read a small heap as its share, which is all but none
// over its base, and lit it through, a flat white blob over the shaded heap
// behind it. Over a longer `foot` the heap's field ramps across that length:
// the share of the piece within it.)
// (And apart: `own`, the heaps' own base, and `share`, their share of the
// air, the cache's, which the forms draw by its own law where they are far,
// cloudHeapsShareSigma.)
// (Where `clBaseBound`, the finer of each map's levels alone: the march's
// bound asks only how far off the forms are, and read over both levels its
// four lookups more a step were a sixth of a view up through the heaps.)
bool clBaseBound = false;
// (How much of the heaps the march draws where it reads: 1 nearer than the
// shells, falling to 0 across their hand-over, past which they are the
// shells' alone and the march's forms are the slabs': clouds.frag's.)
float clHeapsKeep = 1.0;
vec4 clBaseParts(ClAt w, float lod, vec3 pd, float foot, float seen, out float own, out float share) {
    vec3 c = clCacheBase(w, clBaseBound ? floor(lod) : lod);
    share = c.z;
    own = -1.0;
    if (cloudHeapMapSide() <= 0.0) return vec4(c.x, -1.0, c.y, 1.0);
    // (Their share as the field, the bounds' and the face's: in cloud
    // wherever it is any, over the surface's half, so the march goes in and
    // draws it by its law; under 0 it is how far off they are, as the
    // world gives it. As a base, a share of a few tenths was clear air the
    // march stepped past.)
    float sb = c.z > 0.0 ? 0.5 + 0.5 * min(c.z, 1.0) : c.z;
    // (Handed over to the shells, the heaps as themselves are theirs; their
    // share where they are far stays the march's, as the shells leave it.)
    if (clHeapsKeep <= 0.0) {
        vec2 xy = cloudChartAt(normalize(pd));
        float far = cloudHeapsFar(seen, cloudHeapMapTexel(xy, seen));
        if (far <= 0.0) share = 0.0;
        return vec4(c.x, far > 0.0 ? mix(-1.0, sb, far) : -1.0, c.y, far);
    }
    float texel;
    float in_ = cloudHeapMapIn(cloudChartAt(normalize(pd)), seen, w.h * (clouds.shell.y - clouds.shell.x), !clBaseBound, texel);
    float far = cloudHeapsFar(seen, texel);
    own = far < 1.0 ? min(0.5 + in_ / max(clouds.forms.w, foot - seen), 1.0) : 0.0;
    return vec4(c.x, mix(own, sb, far), c.y, far);
}
vec4 clBaseParts(ClAt w, float lod, vec3 pd, float foot, float seen) {
    float own, share;
    return clBaseParts(w, lod, pd, foot, seen, own, share);
}
float clBase(ClAt w, float lod, float foot) {
    vec4 b = clBaseParts(w, lod, cloudDrift(w.p), foot, foot);
    return max(b.x, b.y);
}

// The path a view crosses one far heap in (cloudHeapsSharePath of the
// view's slope through the deck; a heap's depth where no view is: the
// volume's and the cache's bakes).
float clHeapPath = CL_HEAP_SHARE_PATH;

// The forms at a point of the view.
struct ClForms {
    float sigma;    // their extinction
    float face;     // their field less its surface's (cloudFieldOf): > 0 in cloud, < 0 out
    float cover;    // how much of the pixel they fill here (its share across the surface's ramp; all of
                    // it where the surface's blur is a cloud's edge or wider, as the share's extinction says)
    float within;   // the depth within the clouds of what they send (the powder's), where more than the piece's own:
                    // the far heaps' share is a veil as opaque as they cover, but what it sends is a heap's top's
};

// The forms at `w` (`lod` the cache's level there) over footprint `foot`,
// with the detail or without, as a pixel of footprint `seen` shows them
// (how much of the skin's move shows is the pixel's, not the length a
// piece's mean is taken over: cloudSkinAt).
ClForms clFormsAt(ClAt w, float lod, float foot, float seen, bool detail) {
    ClForms f = ClForms(0.0, -1.0, 0.0, 0.0);
    if (w.h <= 0.0 || w.h >= 1.0) return f;
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
    float own, share;
    vec4 bp = clBaseParts(w, lod, pd, foot, seen, own, share);
    vec2 bs = vec2(max(bp.x, bp.y), bp.z);
    float base = max(bs.x, 0.0);
    float far = bp.w;
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
    // (Where the heaps are far, `far` of them their share by its own law,
    // over the slabs': cloudHeapsShareSigma. The field, the face, the march's
    // bounds read, is the base's as it was.)
    if (far <= 0.0) {
        f.sigma = cloudSigmaOf(base, skin, wB, foot, ramp);
    } else {
        float near = far < 1.0 ? cloudSigmaOf(max(max(bp.x, own), 0.0), skin, wB, foot, ramp) : 0.0;
        float slabs = cloudSigmaOf(max(bp.x, 0.0), skin, wB, foot, ramp);
        float heaps = cloudHeapsShareSigma(share, clHeapPath);
        f.sigma = mix(near, max(slabs, heaps), far);
        // (Lit as the tops it stands for: a veil as thin as its sigma sent
        // a wisp's light, so a far field of heaps came in dim, grey.)
        if (heaps > slabs) f.within = far * clouds.forms.x * clHeapPath;
    }
    // (Handed over to the shells as much as they are drawn: the slabs' and
    // this much of the heaps as themselves with them, their share as is.)
    if (clHeapsKeep < 1.0 && clHeapsKeep > 0.0) {
        float slabs = cloudSigmaOf(max(bp.x, 0.0), skin, wB, foot, ramp);
        float asShare = far > 0.0 ? mix(slabs, max(slabs, cloudHeapsShareSigma(share, clHeapPath)), far) : slabs;
        f.sigma = mix(asShare, f.sigma, clHeapsKeep);
    }
    float face = cloudFieldOf(base, skin, wB) + 0.5 * wB;
    f.face = f.sigma > 0.0 ? max(face, 1e-5) : min(face, -1e-5);
    f.cover = f.sigma > 0.0 ? mix(mix(clamp(face / ramp, 0.0, 1.0), 1.0, wB), 1.0, far) : 0.0;
    return f;
}
