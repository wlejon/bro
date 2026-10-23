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
// A cumulus is a soft solid with a skin, so the view is traced, not
// sampled. Through clear air it steps by a distance it knows holds no
// cloud: the most of the cache's distance to cloud and the band of the
// columns about it (the heights that may hold any; both made from where
// the cache's cells may hold cloud however near, CL_B_HOLDS), and the
// billows' own distance field (cloudBillowsClear) at the greatest base
// about the point (the forms only ever take from the billows, so no cloud
// is passed over and no gap's edge torn). Where that distance falls under
// CL_SKIN_PIXELS pixels the view is on a billow's skin: the forms are read
// there, and crossed in steps of that length growing by CL_SKIN_GROWTH to
// CL_SKIN_MOST times it while they hold none; where a billow holds none
// because the detail erodes it (a hole), it is crossed as the inside is.
// Where the view meets cloud, its face is found back toward the last clear
// point by false position on the forms' signed field (sky/scene_cloud_
// layer.glsl's billows less the detail's erosion) to CL_FACE_PIXELS
// footprints, so the face moves with the cloud and not with the steps (and
// so for up to CL_FACES faces beyond holes); inside, pieces from the face
// each CL_IN_GROWTH times the last (fine at the skin, where the detail and
// the powder show; long deeper in) until the view is spent, or until a
// piece is out of the billow, where it traces on to the next cloud. No
// jitter: the
// picture is the same every frame. At most CL_VIEW_STEPS iterations a
// pixel, each one lookup of the forms or of the bounds.
//
// From above, where the deck is a few pixels thick or less (orbit, the far
// deck from the limb), no march: each span is the column's layer from the
// cache (sky/clouds_light.comp), its mean cut to the pixel by the billows,
// lit as one piece; the march blends into it over CL_LAYER_FROM ..
// CL_LAYER_TO.
//
// The light, as the land's, from the cache alone: the sun above the air
// (lighting.atm's illuminance) through the air to the sample (the
// transmittance table, the planet's shadow included) and through the deck
// toward the sun (the cache's depth), with a two-lobed phase and multiple
// scattering's six octaves (Wrenninge 2013), the deeper ones dimmed at a
// cloud's thin edge (the powder); and the sky's light at the deck (the
// atmosphere's SH: above, less what the cloud over the sample hides, and
// the lit ground below, by the height in the deck). The scattering
// integrated over each piece (Hillaire 2016), and the air between the eye
// and the deck from the same tables the land's is (sceneAtmosphereAir), at
// the deck's mean depth along the view, so a far deck hazes into the limb.

layout(location = 0) in vec3 inWorldDir;
layout(location = 0) out vec4 outColor;

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;

// (Define CL_DEBUG_STEPS to see the iterations each pixel takes: three bits
// of red, green and blue, the bins 0, 1-11, 12-23, 24-35, 36-47, 48-71,
// 72-127 and 128 on.)
//#define CL_DEBUG_STEPS
const int CL_VIEW_STEPS = 160;
const int CL_PROBE_LEVELS = 1;     // how much coarser the band probed is than the point's level
const int CL_PROBE_UP = 6;         // how many levels coarser still it may be
const int CL_FREE_LEVELS = 3;      // how much coarser the second distance to cloud read is
const float CL_BASE_MARGIN = 1.04; // the base's at a step's ends times this, plus CL_BASE_PAD,
const float CL_BASE_PAD = 0.004;   // bounds it along the step
const float CL_SKIN_PIXELS = 4.0;  // a step across a billow's skin, in pixels
const float CL_SKIN_GROWTH = 1.5;  // each clear one along a run the last's times this
const float CL_SKIN_MOST = 2.0;    // to at most this many times the first
const float CL_LIP_BILLOWS = 0.3;  // the most the billows' field changes a texel of its noise
const float CL_LIP_BASE = 0.0015;  // the base's, a metre
const float CL_FACE_PIXELS = 1.5;  // how closely (pixels) a face is found
const int CL_FACE_HALVINGS = 5;    // at most
const int CL_FACES = 3;            // faces a view halves for at most
const float CL_IN_PIXELS = 2.0;    // the first piece inside, in pixels
const float CL_IN_FIRST = 12.0;    // and in metres, at least
const float CL_IN_GROWTH = 2.0;    // each the last's times this
const float CL_IN_LONGEST = 400.0; // at most (m)
const float CL_OPAQUE = 0.02;      // what is left of a view that ends it
const float CL_DETAIL_SEEN = 0.3;  // what must be left of a view for the detail to show in it
const float CL_LAYER_FROM = 0.06;  // a pixel's footprint (of the deck's thickness) where the layer begins
const float CL_LAYER_TO = 0.12;    // and where it is all

// What the march is doing.
const int CL_TRACE = 0;   // through clear air, or across a skin
const int CL_HALVE = 1;   // halving toward a face
const int CL_INSIDE = 2;  // through a cloud

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

// How far (m) the view at `w` is sure to meet no cloud, from the cache's
// level `k`: its cell's own (the gap between the cell's box and the nearest
// that holds any), a little less for the chart's stretch.
float clFree(ClAt w, int k) {
    const int B = CL_CACHE_BINS;
    ivec2 c = ivec2(floor(w.xy / cloudCacheCell(k))) & (CL_CACHE_SIDE - 1);
    int b = clamp(int(floor(w.h * float(B))), 0, B - 1);
    return 0.98 * texelFetch(cloudCacheMap, ivec3(c, k * B + b), 0).w;
}

// The forms at a point of the view.
struct ClForms {
    float sigma;    // their extinction
    float face;     // how far over their face (the noise's units): > 0 in cloud, < 0 out
    float billow;   // how far into a billow, before the detail erodes it: < 0 out of one
    float gap;      // how far (m) on they are sure to hold none (0 if not known)
};

// The forms at `w` (`lod` the cache's level there) over footprint `foot`,
// with the detail or without. The gap from how far the billows' noise is
// under its threshold over how fast it (and the base) can change
// (Lipschitz: CL_LIP_BILLOWS a texel, the base CL_LIP_BASE a metre); none
// where the noise is read wider than a texel (its mips' spread then says
// where cloud may be, not how far).
ClForms clFormsAt(ClAt w, float lod, float foot, bool detail) {
    ClForms f = ClForms(0.0, -1.0, -1.0, 0.0);
    if (w.h <= 0.0 || w.h >= 1.0) return f;
    float base = max(clBase(w, lod), 0.0);
    vec3 pd = cloudDrift(w.p);
    vec4 n = cloudBillows(pd, foot);
    // (Under a texel of the billows x does not vary at all, but eight bits
    // a channel leave w - x squared a spread of up to 0.08 there, which
    // blurs a near edge: none at the noise's finest level, all of the next.)
    float texB = clouds.forms.y / float(CL_NOISE_SIDE);
    n.w = mix(n.x * n.x, n.w, clamp(log2(max(foot / texB, 1.0)), 0.0, 1.0));
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
    f.sigma = base > 0.0 ? cloudFormsOf(n, e, wD, base, inCloud, foot) : 0.0;
    float thr = cloudFormsThreshold(n, base);
    // (The share's soft edge holds cloud a little under the threshold.)
    float g = n.x + 0.025 - thr;
    f.billow = f.sigma > 0.0 ? max(g, 0.0) : g;
    // (What the detail erodes, as cloudFormsOf: the billow's depth over
    // the detail's share of it.)
    float fine = e.y * 0.6 + e.z * 0.4;
    fine = mix(1.0 - fine, fine, smoothstep(0.0, 0.2, inCloud));
    float face = n.x - thr - fine * wD * max(1.0 - thr, 0.0);
    f.face = f.sigma > 0.0 ? max(face, 1e-5) : min(face, -1e-5);
    if (g < 0.0 && foot < texB) f.gap = -g / (CL_LIP_BILLOWS / texB + 1.5 * CL_LIP_BASE);
    return f;
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
        vec2 ndc = (vec2(px) + 0.5) / camera.viewport.xy * 2.0 - 1.0;
        vec4 v = camera.invProj * vec4(ndc, depth, 1.0);
        tMax = length(v.xyz / v.w);
    }
    vec3 ro = atmOrigin(vec3(0.0));
    float g = atmGroundHit(ro, rd);
    if (g >= 0.0) tMax = min(tMax, g);
    vec2 spanA, spanB;
    cloudSpans(ro, rd, tMax, spanA, spanB);
    if (spanA.x >= spanA.y && spanB.x >= spanB.y) {
#ifdef CL_DEBUG_STEPS
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
#endif
        discard;
    }

    vec3 s = lighting.atm.sunDir.xyz;
    vec3 E = lighting.atm.sunIlluminance.rgb;
    float nu = dot(rd, s);
    float albedo = clouds.params.x;
    float pixel = clouds.params.y;
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

    int mode = CL_TRACE;
    float t = sp.x;          // where the view is
    float probeEnd = t;      // the band says nothing more before here
    float xPrev = t;         // the furthest the view is known clear to
    float hLo = 0.0, hHi = 0.0;   // the face's bounds while it is halved
    int halvings = 0;
    float len = 0.0;         // inside: the next piece's length
    float hole = -1.0;       // inside: the last clear sample, while the pieces cross a hole
    float clearAt = -1.0, clearFace = 0.0;   // the last clear sample, and the forms' field there
    float fLo = 0.0, fHi = 0.0;             // the field at the face's bounds (fLo 0: not known)
    int side = 0;                            // which bound the last halving moved
    float skinRun = 1.0;                     // the skin's step grows along a run of clear skin samples
    int faces = 0;           // faces halved for
    int steps = 0;
    while (wl < 1.0 && steps < CL_VIEW_STEPS && m.T > CL_OPAQUE) {
        ++steps;
        if (mode == CL_TRACE) {
            if (t >= sp.y) {
                if (span == 1 || spanB.x >= spanB.y) break;
                span = 1;
                sp = spanB;
                t = probeEnd = xPrev = sp.x;
            }
            vec3 a = ro + rd * t;
            ClAt w = clAt(a);
            int k = cloudCacheLevel(w.xy, pixel * t);
            if (k >= CL_CACHE_LEVELS) {   // (past the cache: nothing held)
                t = xPrev = sp.y;
                continue;
            }
            // The probe: the band of the columns about the view's crossing
            // of a coarser cell; where the view crosses it outside the band,
            // it is crossed at once (the coarsest of up to CL_PROBE_UP
            // levels up that says so), and where it is below or above the
            // band, the view goes on from where it meets it.
            float skip = 0.0;
            if (t >= probeEnd) {
                float r = length(a);
                vec3 up = a / r;
                float vs = abs(dot(rd, up));
                float hs = sqrt(max(1.0 - vs * vs, 0.0));
                float tp = -dot(ro, rd);
                float dth = 0.0;
                vec4 band = vec4(0.0);
                for (int j = 0; j < CL_PROBE_UP; ++j) {
                    int kb = k + CL_PROBE_LEVELS + j;
                    if (kb >= CL_CACHE_LEVELS) break;
                    float dj = min(0.5 * cloudCacheCell(kb) / max(hs, 1e-4), sp.y - t);
                    vec4 bj = cloudCacheBand(w.xy, kb);
                    if (j == 0) {
                        dth = dj;
                        band = bj;
                    }
                    float Rlo = mix(clouds.shell.x, clouds.shell.y, bj.x);
                    float Rhi = mix(clouds.shell.x, clouds.shell.y, bj.y);
                    float r1 = length(ro + rd * (t + dj));
                    float rmin = tp > t && tp < t + dj ? length(ro + rd * tp) : min(r, r1);
                    if (!(bj.z < 1e-4 || max(r, r1) < Rlo || rmin > Rhi)) break;
                    skip = dj;
                    if (dj >= sp.y - t) break;
                }
                float Rlo = mix(clouds.shell.x, clouds.shell.y, band.x);
                float Rhi = mix(clouds.shell.x, clouds.shell.y, band.y);
                if (skip <= 0.0) probeEnd = t + dth;
                if (skip <= 0.0 && (r < Rlo || r > Rhi)) {
                    float R = r < Rlo ? Rlo : Rhi;
                    float b = dot(ro, rd), dd = b * b - (dot(ro, ro) - R * R);
                    float tin = r < Rlo ? -b + sqrt(max(dd, 0.0)) : -b - sqrt(max(dd, 0.0));
                    if (dd > 0.0 && tin > t && tin < t + dth) {
                        t = xPrev = tin;
                        a = ro + rd * t;
                        w = clAt(a);
                    }
                }
            }
            float foot = pixel * t;
            float lod = clLod(w, foot);
            // How far the view is sure to meet no cloud: the cache's distance
            // (this level's, or a coarser one's where that says more, less a
            // texel's reach: a lookup blends its texels'), and the billows'
            // own at the greatest base about the point.
            float dFree = clFree(w, k);
            int kc = min(k + CL_FREE_LEVELS, CL_CACHE_LEVELS - 1);
            if (kc > k) dFree = max(dFree, clFree(w, kc));
            // (The base is smooth over a step, so the greater of its values
            // at the step's ends, with a margin, bounds it along the step.)
            vec3 pd = cloudDrift(w.p);
            float b0 = clBase(w, lod) * CL_BASE_MARGIN + CL_BASE_PAD;
            float dB = cloudBillowsClear(pd, b0, foot);
            float skin = max(CL_SKIN_PIXELS * foot, 1.0);
            float skinStep = skin * skinRun;
            if (dB > skin) {
                ClAt we = clAt(ro + rd * (t + dB));
                float b1 = clBase(we, clLod(we, pixel * (t + dB))) * CL_BASE_MARGIN + CL_BASE_PAD;
                if (b1 > b0) dB = cloudBillowsClear(pd, b1, foot);
            }
            float d = max(dFree, dB);
            if (max(d, skip) > skin) {
                t += max(d, skip);
                xPrev = t;
                skinRun = 1.0;
                continue;
            }
            // On a billow's skin: the forms here, and how far on they are
            // sure to hold none (the step growing along a run of clear ones).
            ClForms f = clFormsAt(w, lod, foot, m.T > CL_DETAIL_SEEN);
            if (f.sigma <= 0.0 && f.billow < 0.0) {
                float sure = max(d, f.gap);
                clearAt = t;
                clearFace = f.face;
                xPrev = sure >= skinStep ? t + sure : t;
                t += max(sure, skinStep);
                skinRun = min(skinRun * CL_SKIN_GROWTH, CL_SKIN_MOST);
                continue;
            }
            if (f.sigma <= 0.0) {
                // In a billow the detail has eroded: through it as through
                // the cloud, its face found where the pieces meet some.
                hole = t;
                clearAt = t;
                clearFace = f.face;
                mode = CL_INSIDE;
                len = max(CL_IN_PIXELS * foot, CL_IN_FIRST);
                continue;
            }
            // The view meets cloud: its face lies between the last clear
            // point and here.
            hLo = max(xPrev, sp.x);
            hHi = t;
            fLo = hLo == clearAt ? clearFace : 0.0;
            fHi = f.face;
            halvings = clamp(int(ceil(log2(max((hHi - hLo) / (CL_FACE_PIXELS * foot), 1.0)))), 0, CL_FACE_HALVINGS);
            if (halvings > 0) {
                mode = CL_HALVE;
            } else {
                mode = CL_INSIDE;
                len = max(CL_IN_PIXELS * foot, CL_IN_FIRST);
            }
            continue;
        }
        if (mode == CL_HALVE) {
            // The face by false position on the forms' signed field (where
            // its value at the near end is known; halving else), each end
            // that stays put twice halved in weight (Illinois), to
            // CL_FACE_PIXELS footprints.
            float s = fLo < 0.0 ? clamp(fLo / (fLo - fHi), 0.1, 0.9) : 0.5;
            float x = mix(hLo, hHi, s);
            ClAt w = clAt(ro + rd * x);
            float foot = pixel * x;
            ClForms f = clFormsAt(w, clLod(w, foot), foot, m.T > CL_DETAIL_SEEN);
            if (f.sigma > 0.0) {
                hHi = x;
                fHi = f.face;
                if (side == 1) fLo *= 0.5;
                side = 1;
            } else {
                hLo = x;
                fLo = f.face;
                if (side == -1) fHi *= 0.5;
                side = -1;
            }
            if (--halvings <= 0 || hHi - hLo < CL_FACE_PIXELS * foot) {
                t = fLo < 0.0 ? mix(hLo, hHi, clamp(fLo / (fLo - fHi), 0.0, 1.0)) : 0.5 * (hLo + hHi);
                side = 0;
                mode = CL_INSIDE;
                len = max(CL_IN_PIXELS * pixel * t, CL_IN_FIRST);
            }
            continue;
        }
        // Inside: the next piece, sampled at its middle.
        if (t >= sp.y) {
            mode = CL_TRACE;
            continue;
        }
        float L = min(len, sp.y - t);
        float x = t + 0.5 * L;
        ClAt w = clAt(ro + rd * x);
        float foot = pixel * x;
        float lod = clLod(w, foot);
        ClForms f = clFormsAt(w, lod, foot, m.T > CL_DETAIL_SEEN);
        float sigma = f.sigma;
        if (sigma <= 0.0) {
            clearAt = x;
            clearFace = f.face;
            if (f.billow < 0.0) {
                // Out of the billow: traced on from here.
                t = xPrev = x;
                mode = CL_TRACE;
                continue;
            }
            // A hole the detail eroded: crossed (at once as far as the
            // cache's distance to cloud says), the face beyond it found when
            // the pieces meet cloud again.
            hole = x;
            int kh = cloudCacheLevel(w.xy, foot);
            float dh = kh < CL_CACHE_LEVELS ? clFree(w, kh) : 0.0;
            if (dh > L) {
                t = hole = x + dh;
                continue;
            }
            t += L;
            len = min(len * CL_IN_GROWTH, CL_IN_LONGEST);
            continue;
        }
        if (hole >= 0.0 && m.T > CL_DETAIL_SEEN && faces < CL_FACES) {
            // Out of a hole into cloud the eye sees: its face halved for.
            hLo = hole;
            hHi = x;
            fLo = hole == clearAt ? clearFace : 0.0;
            fHi = f.face;
            hole = -1.0;
            ++faces;
            halvings = clamp(int(ceil(log2(max((hHi - hLo) / (CL_FACE_PIXELS * foot), 1.0)))), 0, CL_FACE_HALVINGS);
            if (halvings > 0) {
                mode = CL_HALVE;
                continue;
            }
        }
        hole = -1.0;
        if (!sky) {
            clSkyAt(ro, rd * x, skyAbove, groundBelow);
            sky = true;
        }
        // (The powder's depth over the pixel's footprint, not the piece's
        // length, which grows from the face.)
        clIntegrate(m, ro + rd * x, x, L, sigma, clCache(w, lod).yz, sigma * max(120.0, foot), s, nu, ph, albedo, E,
                    skyAbove, groundBelow);
        t += L;
        len = min(len * CL_IN_GROWTH, CL_IN_LONGEST);
    }
    if (wl > 0.0) {
        m.L = mix(m.L, ml.L, wl);
        m.T = mix(m.T, ml.T, wl);
        m.depthSum = mix(m.depthSum, ml.depthSum, wl);
        m.weightSum = mix(m.weightSum, ml.weightSum, wl);
    }
    float alpha = 1.0 - m.T;
#ifdef CL_DEBUG_STEPS
    int bin = steps == 0 ? 0 : steps < 12 ? 1 : steps < 24 ? 2 : steps < 36 ? 3 : steps < 48 ? 4 : steps < 72 ? 5
            : steps < 128 ? 6 : 7;
    // (The HDR target is in physical units: a bin's colour scaled up to show.)
    outColor = vec4(vec3(bin & 1, (bin >> 1) & 1, bin >> 2) * 1e6, 1.0);
    return;
#endif
    if (alpha < 1.0 / 1024.0) discard;
    // The air between the eye and the deck, at its mean depth.
    vec3 airT, airL;
    sceneAtmosphereAir(rd * (m.depthSum / max(m.weightSum, 1e-6)), airT, airL);
    vec3 col = m.L * airT + alpha * airL;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha);
}
