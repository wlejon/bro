#version 450
// The cloud deck (vulkan/sky/pass_clouds.h; docs/scene-world-architecture.md
// §13 "volumes", §11 "going up"): each pixel's view marched through the
// deck's shell, from below, inside and above it the same way, up to the
// opaque surfaces' depth (a hill before a cloud hides it, a cloud before a
// hill hides the hill), and blended over what is drawn.
//
// The march never runs the world's module: it reads the deck's cache about
// the eye's nadir (sky/clouds_layout.h): the world's base, the depths from
// there toward the sun and up, how far the nearest cloud is and each
// column's band; and cuts the base into the engine's forms at the pixel's
// footprint (sky/scene_cloud_layer.glsl's cloudFormsOf: the billows' noise,
// and the detail's while what is seen of the view is still mostly ahead).
// Each lookup of the cache blends the two levels about the footprint (or
// about the finest whose window holds the point), so no level gives way to
// the next along a line.
//
// One loop, one evaluation of the forms an iteration, whatever the march is
// doing (a step's probe, a leaf, a face's halving, the way toward the sun),
// so the pixels of a group run the same costly part together whichever of
// these each is at. The view crosses the deck in at most two spans. A step:
// a cell across (CL_BASE_CELL at least) and a bin up, crossed at once where
// the band of the columns about it holds no cloud over the view's heights,
// where the cache's distance to cloud reaches past it, or where the billows
// read over the whole of it fill next to none. Else it is cut into leaves
// CL_LEAF_PIXELS pixels long (CL_LEAVES at most), each sampled once; a leaf
// that holds next to nothing is lit at once from the cache (a wisp). Where
// the view enters cloud, its face is found by halving to CL_FACE_PIXELS
// footprints, so a face moves with the cloud and not with the leaves' grid
// (which would show as contours on a deck's top and streaks along a base).
// No jitter: the picture is the same every frame. At most CL_VIEW_STEPS
// iterations a pixel.
//
// From above, where the deck is a few pixels thick or less (orbit, the far
// deck from the limb), no march: each span is the column's layer from the
// cache (sky/clouds_light.comp), its mean cut to the pixel by the billows,
// lit as one piece; the march blends into it over CL_LAYER_FROM ..
// CL_LAYER_TO.
//
// The light, as the land's: the sun above the air (lighting.atm's
// illuminance) through the air to the sample (the transmittance table, the
// planet's shadow included) and through the deck toward the sun (its first
// few hundred metres through the forms themselves, the rest the cache's
// depth from there), with a two-lobed phase and multiple scattering's six
// octaves (Wrenninge 2013), the deeper ones dimmed at a cloud's thin edge
// (the powder); and the sky's light at the deck (the atmosphere's SH:
// above, less what the cloud over the sample hides, and the lit ground
// below, by the height in the deck). The scattering integrated over each
// piece (Hillaire 2016), and the air between the eye and the deck from the
// same tables the land's is (sceneAtmosphereAir), at the deck's mean depth
// along the view, so a far deck hazes into the limb.

layout(location = 0) in vec3 inWorldDir;
layout(location = 0) out vec4 outColor;

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;

// (Define CL_DEBUG_STEPS to see the iterations each pixel takes as hues.)
const int CL_VIEW_STEPS = 256;
const int CL_PROBE_LEVELS = 1;    // how much coarser the band probed is than the step's level
const int CL_FREE_LEVELS = 3;     // how much coarser the second distance to cloud read is
const int CL_LEAVES = 8;          // a step's leaves, at most
const float CL_LEAF_PIXELS = 4.0; // a leaf's length, in pixels up the screen
const float CL_OPAQUE = 0.02;     // what is left of a view that ends it
const float CL_DETAIL_SEEN = 0.3; // what must be left of a view for the detail to show in it
const float CL_BASE_CELL = 400.0; // the shortest stretch (m) a step crosses of the base
const float CL_SHARE_CLEAR = 0.01;// the most of a step's air its billows may fill for it to be crossed as clear
const float CL_FACE_PIXELS = 1.5; // how closely (pixels) a face is found
const int CL_SUN_STEPS = 3;       // the forms' samples toward the sun from a lit point
const float CL_SUN_FIRST = 40.0;  // the first's length (m)
const float CL_SUN_GROWTH = 3.0;  // each the last's times this
const float CL_LAYER_FROM = 0.06; // a pixel's footprint (of the deck's thickness) where the layer begins
const float CL_LAYER_TO = 0.12; // and where it is all

// What the march is doing.
const int CL_STEP = 0;    // a step's probe
const int CL_LEAF = 1;    // a leaf
const int CL_HALVE = 2;   // halving toward a face
const int CL_SUN = 3;     // toward the sun from a lit point

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

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

// The cache's level about footprint `foot` at `w`, as a fraction: its
// whole part the finer of the two read (whose window holds the point), the
// rest how far toward the coarser.
float clLod(ClAt w, float foot) {
    vec2 dn = abs(w.xy - clouds.cacheNadir.xy);
    float far = max(dn.x, dn.y) / (CL_CACHE_MARGIN * CL_CACHE_TEXEL0);
    return max(max(log2(max(foot, 1e-3) / CL_CACHE_TEXEL0), log2(max(far, 1e-6)) + 1.0), 0.0);
}

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
// The base alone, from the cache's narrower image (its texels half the
// bytes: what every sample reads; the depths only what is lit does).
float clBase(ClAt w, float lod) {
    int k = int(lod);
    if (k >= CL_CACHE_LEVELS) return 0.0;
    float a = textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k), 0.0).g;
    float f = fract(lod);
    if (k + 1 >= CL_CACHE_LEVELS || f <= 0.0) return a;
    return mix(a, textureLod(cloudSigmaMap, cloudCacheCoord(w.xy, w.h, k + 1), 0.0).g, f);
}

// The forms at `w` (`lod` the cache's level there) over footprint `foot`:
// their extinction, with the detail or without, and `share`, the share of
// the footprint the billows fill.
float clForms(ClAt w, float lod, float foot, bool detail, out float share) {
    share = 0.0;
    if (w.h <= 0.0 || w.h >= 1.0) return 0.0;
    float base = clBase(w, lod);
    if (base <= 0.0) return 0.0;
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
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
    share = cloudFormsShare(n, base);
    return cloudFormsOf(n, e, wD, base, inCloud, foot);
}

// What the march carries along a view.
struct ClMarch {
    vec3 L;          // light scattered toward the eye
    float T;         // what is left of the view
    float depthSum;  // distance weighted by what each piece took
    float weightSum;
};

// The sky's light at air-space offset `at` from the eye `ro`: from above,
// and the lit ground's from below.
void clSkyAt(vec3 ro, vec3 at, out vec3 skyAbove, out vec3 groundBelow) {
    vec3 atUp = normalize(ro + at);
    vec3 shc[6];
    mat3 shFrame;
    atmSkyLightSh(at, shc, shFrame);
    skyAbove = atmShIrradiance(shc, shFrame, atUp);
    groundBelow = atmShIrradiance(shc, shFrame, -atUp);
}

// One piece's light, integrated over its length `dt` (Hillaire 2016): `tau`
// the deck's depths from the point lit toward the sun and up, `about` the
// cloud's optical depth about the point (the powder's), `ph` the octaves'
// phases.
void clIntegrate(inout ClMarch m, vec3 p, float tm, float dt, float sigma, vec2 tau, float about, vec3 s,
                 float nu, float ph[6], float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow) {
    float r = length(p);
    vec3 up = p / r;
    float h = clamp((r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x), 0.0, 1.0);
    // The sun at the sample: through the air (and the planet's shadow),
    // then through the deck.
    vec3 sunAir = atmSunTransmittance(r, dot(up, s));
    // Multiple scattering's octaves (Wrenninge), the deeper ones dimmed
    // where the cloud is thin about the sample (the powder: little light
    // has scattered about at a cloud's edge, so seen with the sun behind
    // the eye its edges are darker than its body; toward the sun, the edge
    // is the bright silver lining).
    // (Six octaves, each's extinction 0.4 of the last's and its light 0.8:
    // a thick cloud seen from the sun's side sends back about what a white
    // diffuser would, as a deck from orbit does; four at 0.5 and 0.6 left
    // it grey.)
    float powder = mix(1.0 - exp(-about), 1.0, clamp(nu, 0.0, 1.0));
    float ms = 0.0, a = 1.0, b = 1.0;
    for (int o = 0; o < 6; ++o) {
        ms += b * ph[o] * exp(-tau.x * a) * (o == 0 ? mix(1.0, powder, 0.5) : powder);
        a *= 0.4;
        b *= 0.8;
    }
    // The sky's light, less what the cloud above hides (a floor for what
    // its body scatters about), and the ground's from below.
    float sees = mix(0.2, 1.0, exp(-0.15 * tau.y));
    vec3 ambient = skyAbove * (0.35 + 0.65 * h) * sees + groundBelow * (0.6 - 0.4 * h);
    vec3 S = albedo * sigma * (E * sunAir * ms + ambient);
    float Tr = exp(-sigma * dt);
    m.L += m.T * (S - S * Tr) / sigma;
    float took = m.T * (1.0 - Tr);
    m.depthSum += took * tm;
    m.weightSum += took;
    m.T *= Tr;
}

void main() {
    vec3 rd = normalize(inWorldDir);
    ivec2 px = ivec2(gl_FragCoord.xy);
    // The opaque surface's distance along the view (reversed-Z: 0 the far plane, the sky).
    float depth = texelFetch(cloudDepth, px, 0).r;
    float tMax = 1e30;
    if (depth > 0.0) {
        vec2 ndc = gl_FragCoord.xy / camera.viewport.xy * 2.0 - 1.0;
        vec4 v = camera.invProj * vec4(ndc, depth, 1.0);
        tMax = length(v.xyz / v.w);
    }
    vec3 ro = atmOrigin(vec3(0.0));
    float g = atmGroundHit(ro, rd);
    if (g >= 0.0) tMax = min(tMax, g);
    vec2 spanA, spanB;
    cloudSpans(ro, rd, tMax, spanA, spanB);
    if (spanA.x >= spanA.y && spanB.x >= spanB.y) discard;

    vec3 s = lighting.atm.sunDir.xyz;
    vec3 E = lighting.atm.sunIlluminance.rgb;
    float nu = dot(rd, s);
    float albedo = clouds.params.x;
    float pixel = clouds.params.y;
    float binH = clouds.cacheE1.w;
    float heart = clouds.forms.x;
    float ph[6];
    for (int o = 0; o < 6; ++o) ph[o] = clPhase(nu, exp2(-float(o)));

    // The sky's light at the deck, above it and from the ground below:
    // where the view first meets a cloud, once it does.
    vec3 skyAbove = vec3(0.0), groundBelow = vec3(0.0);
    bool sky = false;

    ClMarch m = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    int span = spanA.x < spanA.y ? 0 : 1;
    vec2 sp = span == 0 ? spanA : spanB;

    // From above, where the deck is a few pixels thick or less: each span
    // the column's layer where the view enters it (sky/clouds_light.comp),
    // its depth along the view the column's slanted over the span, lit at
    // the mean depth of what it sends; the march blended out over
    // CL_LAYER_FROM .. CL_LAYER_TO of the deck's thickness a pixel.
    float thick = clouds.shell.y - clouds.shell.x;
    float wl = length(ro) > clouds.shell.y ? smoothstep(CL_LAYER_FROM, CL_LAYER_TO, pixel * sp.x / thick) : 0.0;
    ClMarch ml = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    for (int i = 0; i < 2 && wl > 0.0; ++i) {
        vec2 q = i == 0 ? spanA : spanB;
        if (q.x >= q.y) continue;
        ClAt w = clAt(ro + rd * q.x);
        float lod = clLod(w, pixel * q.x);
        int k = int(lod);
        if (k >= CL_CACHE_LEVELS) continue;
        vec4 col = cloudCacheColumn(w.xy, k);
        if (k + 1 < CL_CACHE_LEVELS) col = mix(col, cloudCacheColumn(w.xy, k + 1), fract(lod));
        float len = q.y - q.x;
        // (The cell's mean cut to the pixel: as much more or less as the
        // billows fill of the pixel's footprint than of the cell's, at the
        // base where the column sends its light from.)
        ClAt wm = clAt(ro + rd * (q.x + len * (1.0 - col.z)));
        wm.h = col.z;
        float base = clBase(wm, lod);
        vec3 pd = cloudDrift(wm.p);
        float shareCell = cloudFormsShare(cloudBillows(pd, cloudCacheCell(k) * exp2(fract(lod))), base);
        float sharePixel = cloudFormsShare(cloudBillows(pd, pixel * q.x), base);
        float tauView = col.x * len / thick * clamp(sharePixel / max(shareCell, 1e-3), 0.0, 4.0);
        if (tauView < 1e-4) continue;
        float at = q.x + len * clamp(col.y / max(col.x, 1e-4), 0.0, 1.0);
        vec3 pl = ro + rd * at;
        float mu = max(dot(normalize(pl), s), 0.1);
        if (!sky) {
            clSkyAt(ro, rd * at, skyAbove, groundBelow);
            sky = true;
        }
        // (The powder's depth the column's own: what it sends comes from
        // cloud as deep as it is, however thin the view's mean.)
        clIntegrate(ml, pl, at, len, tauView / len, vec2(col.y / mu, col.y), col.x, s, nu, ph, albedo, E,
                    skyAbove, groundBelow);
    }
    int mode = CL_STEP;
    float t = sp.x;            // where the next step starts
    float probeEnd = t;        // the band says nothing more before here
    bool clear = true;         // the last piece held next to no cloud
    // The step: its level, end and length; its leaves' length and where the
    // next starts; the furthest the view is known clear to (the last sample
    // in no cloud, or the end of what was crossed as clear).
    int stepK = 0;
    float dt = 0.0, stepEnd = t, d0 = 0.0, a0 = t, xPrev = t;
    // The piece in cloud: its leaf (from l0, dLeaf long), the face's bounds
    // while it is halved, its extinction; where it begins, its length and
    // where it is lit; the depth toward the sun gathered so far.
    float l0 = 0.0, dLeaf = 0.0, hLo = 0.0, hHi = 0.0, pSigma = 0.0, pLo = 0.0, pLen = 0.0, litAt = 0.0;
    float tn = 0.0, sunAt = 0.0, sunL = 0.0;
    int halvings = 0, sunI = 0;
    int steps = 0;
    while (wl < 1.0 && steps < CL_VIEW_STEPS && m.T > CL_OPAQUE) {
        ++steps;
        // 1. The sample this iteration takes.
        float x;
        vec3 pa;
        float pf;
        bool pDetail = false;
        if (mode == CL_STEP) {
            if (t >= sp.y) {
                if (span == 1 || spanB.x >= spanB.y) break;
                span = 1;
                sp = spanB;
                t = probeEnd = xPrev = sp.x;
                clear = true;
            }
            vec3 a = ro + rd * t;
            float r = length(a);
            vec3 up = a / r;
            vec2 xy = cloudChartAt(cloudToBody(up));
            stepK = cloudCacheLevel(xy, pixel * t);
            if (stepK >= CL_CACHE_LEVELS) {   // (past the cache: nothing held)
                t = xPrev = sp.y;
                continue;
            }
            float vs = abs(dot(rd, up));
            float hs = sqrt(max(1.0 - vs * vs, 0.0));
            // The probe: the band of the columns about the view's crossing
            // of the next coarser cell; where the view crosses it outside
            // the band, it is crossed at once, and where it is below or
            // above the band, the view goes on from where it meets it.
            if (t >= probeEnd) {
                int kb = min(stepK + CL_PROBE_LEVELS, CL_CACHE_LEVELS - 1);
                float dth = min(0.5 * cloudCacheCell(kb) / max(hs, 1e-4), sp.y - t);
                vec4 band = cloudCacheBand(xy, kb);
                float Rlo = mix(clouds.shell.x, clouds.shell.y, band.x);
                float Rhi = mix(clouds.shell.x, clouds.shell.y, band.y);
                float r1 = length(ro + rd * (t + dth));
                float tp = -dot(ro, rd);
                float rmin = tp > t && tp < t + dth ? length(ro + rd * tp) : min(r, r1);
                if (band.z < 1e-4 || max(r, r1) < Rlo || rmin > Rhi) {
                    t += dth;
                    xPrev = t;
                    clear = true;
                    continue;
                }
                probeEnd = t + dth;
                if (r < Rlo || r > Rhi) {
                    float R = r < Rlo ? Rlo : Rhi;
                    float b = dot(ro, rd), dd = b * b - (dot(ro, ro) - R * R);
                    float tin = r < Rlo ? -b + sqrt(max(dd, 0.0)) : -b - sqrt(max(dd, 0.0));
                    if (dd > 0.0 && tin > t && tin < t + dth) t = xPrev = tin;
                }
            }
            // A step three quarters of a cell across (of CL_BASE_CELL at
            // least: the base is smooth, its forms are the leaves') and of a
            // bin up, its billows read over all of it.
            float cell = max(cloudCacheCell(stepK), CL_BASE_CELL);
            dt = min(0.75 * min(cell / max(hs, 1e-4), binH / max(vs, 1e-4)), sp.y - t);
            if (dt <= 0.0) {
                t = sp.y;
                continue;
            }
            x = t + 0.5 * dt;
            pf = max(0.5 * dt, pixel * x);
            pa = ro + rd * x;
        } else if (mode == CL_LEAF) {
            dLeaf = min(d0, stepEnd - a0);
            x = a0 + 0.5 * dLeaf;
            pf = pixel * x;
            pDetail = m.T > CL_DETAIL_SEEN;
            pa = ro + rd * x;
        } else if (mode == CL_HALVE) {
            x = 0.5 * (hLo + hHi);
            pf = pixel * x;
            pDetail = m.T > CL_DETAIL_SEEN;
            pa = ro + rd * x;
        } else {
            x = litAt;
            pf = max(pixel * litAt, 0.5 * sunL);
            pa = ro + rd * litAt + s * (sunAt + 0.5 * sunL);
        }

        // 2. The forms there.
        ClAt w = clAt(pa);
        float lod = clLod(w, pf);
        float share;
        float sigma = clForms(w, lod, pf, pDetail, share);

        // 3. What they say.
        bool lit = false;     // a piece in cloud is found: light it
        bool done = false;    // its light is known: add it
        bool after = false;   // on past the piece
        vec2 tau = vec2(0.0);
        if (mode == CL_STEP) {
            // The distance to cloud: this level's (CL_CACHE_REACH cells at
            // most), or a coarser one's where that says more (its cells
            // hold cloud wherever this one's do, so its distance is as
            // safe), less a texel's reach (a lookup blends its texels').
            vec4 c = clCache(w, lod);
            float reachK = max(cloudCacheCell(stepK), binH);
            float dFree = c.w - reachK;
            int kc = min(stepK + CL_FREE_LEVELS, CL_CACHE_LEVELS - 1);
            if (kc > stepK)
                dFree = max(dFree, textureLod(cloudCacheMap, cloudCacheCoord(w.xy, w.h, kc), 0.0).w -
                                       cloudCacheCell(kc) - binH);
            if (c.x <= 1e-3 || dFree > reachK) {
                t += max(dt, 0.5 * dt + dFree);
                xPrev = t;
                clear = true;
                continue;
            }
            if (share < CL_SHARE_CLEAR) {
                t += dt;
                xPrev = t;
                clear = true;
                continue;
            }
            stepEnd = t + dt;
            int n = clamp(int(ceil(dt / max(CL_LEAF_PIXELS * pixel * x, 1.0))), 1, CL_LEAVES);
            d0 = dt / float(n);
            a0 = t;
            mode = CL_LEAF;
        } else if (mode == CL_LEAF) {
            bool entered = clear;
            clear = sigma * dLeaf < 0.01;
            l0 = a0;
            a0 += dLeaf;
            float xLast = xPrev;
            xPrev = x;
            // (Every leaf as short as the step's: a longer one where the
            // last held next to nothing, sampled once at its middle, would
            // pass over the edge of a cloud in some pixels and not in their
            // neighbours, which shows as streaks along the gaps.)
            if (sigma <= 0.0) {
                after = true;
            } else if (clear) {
                // (A wisp: its light at once, from the cache's depths, with
                // no face to find.)
                pSigma = sigma;
                pLo = l0;
                pLen = dLeaf;
                litAt = x;
                tau = clCache(w, lod).yz;
                done = true;
            } else if (entered) {
                // The view enters cloud: where, by halving the forms
                // between the last sample (which held next to none) and
                // this one.
                pSigma = sigma;
                hHi = x;
                hLo = max(xLast, sp.x);
                halvings = clamp(int(ceil(log2(max((hHi - hLo) / (CL_FACE_PIXELS * pixel * x), 1.0)))), 2, 6);
                mode = CL_HALVE;
            } else {
                pSigma = sigma;
                pLo = l0;
                pLen = dLeaf;
                lit = true;
            }
        } else if (mode == CL_HALVE) {
            if (sigma >= 0.5 * pSigma) hHi = x;
            else hLo = x;
            if (--halvings <= 0) {
                pLo = 0.5 * (hLo + hHi);
                pLen = l0 + dLeaf - pLo;
                lit = true;
            }
        } else {
            tn += sigma * sunL;
            sunAt += sunL;
            sunL *= CL_SUN_GROWTH;
            if (++sunI >= CL_SUN_STEPS) {
                // The rest of the way the cache's, from there; up, the
                // cache's a bin over the lit point.
                vec3 pl = ro + rd * litAt;
                float fl = pixel * litAt;
                ClAt ws = clAt(pl + s * sunAt), wu = clAt(pl + normalize(pl) * binH);
                tau = vec2(tn + clCache(ws, clLod(ws, max(fl, sunAt))).y, clCache(wu, clLod(wu, fl)).z + 1.0);
                done = true;
            }
        }
        if (lit) {
            // (A piece's light a free path in when it is thicker than that,
            // else at its middle: what an opaque piece sends comes from its
            // near edge, not from wherever its middle fell.)
            litAt = pSigma * pLen > 1.0 ? pLo + 1.0 / pSigma : pLo + 0.5 * pLen;
            if (m.T > CL_DETAIL_SEEN) {
                // The depth toward the sun from what is seen of the piece:
                // its first few hundred metres through the forms themselves
                // (a billow shades the one behind it, and a face is lit by
                // its own shape, not by the cache's bins, whose layers it
                // would show as stripes).
                tn = 0.0;
                sunAt = 0.0;
                sunL = CL_SUN_FIRST;
                sunI = 0;
                mode = CL_SUN;
            } else {
                // (Deep in what is seen: the cache's depths there.)
                ClAt wl = clAt(ro + rd * litAt);
                tau = clCache(wl, clLod(wl, pixel * litAt)).yz;
                done = true;
            }
        }
        if (done) {
            if (!sky) {
                clSkyAt(ro, rd * litAt, skyAbove, groundBelow);
                sky = true;
            }
            // (The powder's depth over the pixel's footprint, not the
            // piece's length, which differs from pixel to pixel where a face
            // is found by halving and would show as ripples on thin cloud.)
            clIntegrate(m, ro + rd * litAt, litAt, pLen, pSigma, tau, pSigma * max(120.0, pixel * litAt), s, nu, ph,
                        albedo, E, skyAbove, groundBelow);
            after = true;
        }
        if (after) {
            if (a0 >= stepEnd - 1e-3 * d0) {
                t = stepEnd;
                mode = CL_STEP;
            } else {
                mode = CL_LEAF;
            }
        }
    }
    if (wl > 0.0) {
        m.L = mix(m.L, ml.L, wl);
        m.T = mix(m.T, ml.T, wl);
        m.depthSum = mix(m.depthSum, ml.depthSum, wl);
        m.weightSum = mix(m.weightSum, ml.weightSum, wl);
    }
    float alpha = 1.0 - m.T;
#ifdef CL_DEBUG_STEPS
    // (The iterations a pixel takes as hues: < 4 blue, < 8 cyan, < 16
    // green, < 32 yellow, < 64 red, < 128 magenta, more white.)
    const vec3 hue[7] = vec3[7](vec3(0, 0, 1), vec3(0, 1, 1), vec3(0, 1, 0), vec3(1, 1, 0), vec3(1, 0, 0),
                                vec3(1, 0, 1), vec3(1, 1, 1));
    int bDbg = steps < 4 ? 0 : steps < 8 ? 1 : steps < 16 ? 2 : steps < 32 ? 3 : steps < 64 ? 4 : steps < 128 ? 5 : 6;
    outColor = vec4(hue[bDbg] * 1e6, 1.0);
    return;
#endif
    if (alpha < 1.0 / 1024.0) discard;
    // The air between the eye and the deck, at its mean depth.
    vec3 airT, airL;
    sceneAtmosphereAir(rd * (m.depthSum / max(m.weightSum, 1e-6)), airT, airL);
    vec3 col = m.L * airT + alpha * airL;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha);
}
