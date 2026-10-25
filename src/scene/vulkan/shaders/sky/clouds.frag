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
// A cloud is a soft solid with a skin, so the view is traced, not sampled.
// The world's base is the solid (its surface where it reads a half, rising
// across it over forms.edge metres); the engine's billows and detail only
// move that surface, by at most CLOUD_SKIN of the base, so they never open
// a hole through a cloud. Through clear air the view steps by a distance
// it knows holds no cloud: the most of the cache's distance to cloud, the
// band of the columns about it (the heights that may hold any; both made
// from where the cache's cells may hold cloud however near, CL_B_HOLDS),
// and how far the base must still rise before even the skin's most out
// reaches the surface (clBaseClear). Where that falls under CL_SKIN_PIXELS
// pixels the view is on a skin: the forms are read every CL_SKIN_PIXELS
// pixels, never more, so no edge is stepped over and torn. Where the view
// meets cloud, its face is found back toward the last clear point by false
// position on the forms' field (sky/scene_cloud_layer.glsl's cloudFieldOf)
// to CL_FACE_PIXELS footprints, so the face moves with the cloud and not
// with the steps; inside, pieces from the face each CL_IN_GROWTH times the
// last (fine at the skin, where the detail and the powder show; long deeper
// in) until the view is spent or leaves the cloud, where it traces on to
// the next one (halving for up to CL_FACES faces). No jitter: the picture
// is the same every frame. At most CL_VIEW_STEPS iterations a pixel, each
// one lookup of the forms or of the bounds.
//
// From above, where a pixel spans CL_LAYER_TO of the deck's thickness or
// more (orbit, the limb, a climb from a region's height), no march: each
// span is a column's layer (scene_cloud_layer.glsl's cloudColumnLayer: the
// clouds' cover over it, not its heights' sum), lit as one piece. The
// cache's (sky/clouds_light.comp), its mean cut to the pixel by the
// billows, where its cells are as fine as the globe's or finer and the
// pixel finer than the clouds; else the globe's (sky/clouds_globe.comp),
// the footprint's mean of the world's share, so the cache's cut of a share
// into clouds a few of its cells wide never shows from above. The march
// blends into it over CL_LAYER_FROM .. CL_LAYER_TO.
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
// (The clouds' field from their own noise, sky/scene_cloud_layer.glsl's;
// the lighting's copy of it is for the land's shadow.)
#define SCENE_NO_CLOUD_FIELD
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;

// (Define CL_DEBUG_STEPS to see the iterations each pixel takes: three bits
// of red, green and blue, the bins 0, 1-11, 12-23, 24-35, 36-47, 48-71,
// 72-127 and 128 on.)
//#define CL_DEBUG_STEPS
//#define CL_DEBUG_KINDS
//#define CL_DEBUG_TAU
// (CL_DEBUG_OUT: the march's 1 alpha, 2 reflectance of a white diffuser's,
// 3 that over its alpha, in the bins its block lists.)
//#define CL_DEBUG_OUT 3
//#define CL_DEBUG_COUNT dbgSkin
//#define CL_DEBUG_LAYER 3
// (CL_DEBUG_VOLUME, the CloudsVolumeFrag variant: the volume about the eye
// drawn instead of the forms, or its channels as slices, by clouds.volE2.w:
// sky/cloud_volume_debug.glsl.)
const int CL_VIEW_STEPS = 160;
const int CL_PROBE_LEVELS = 1;     // how much coarser the band probed is than the point's level
const int CL_PROBE_UP = 6;   // how many levels coarser still it may be
const int CL_FREE_LEVELS = 3;      // how much coarser the second distance to cloud read is
const float CL_BASE_MARGIN = 1.04; // the base's at a step's ends times this, plus CL_BASE_PAD,
const float CL_BASE_PAD = 0.004;   // bounds it along the step
const float CL_SKIN_PIXELS = 8.0;// a step across a cloud's skin, in pixels
const float CL_FACE_PIXELS = 1.5;  // how closely (pixels) a face is found
const int CL_FACE_HALVINGS = 4;    // at most
const int CL_FACES = 3;            // faces a view halves for at most
const float CL_IN_PIXELS = 2.0;    // the first piece inside, in pixels
const float CL_IN_FIRST = 20.0; // and in metres, at least
const float CL_IN_GROWTH = 2.0;  // each the last's times this
const float CL_IN_LONGEST = 400.0;
const float CL_SUN_AGAIN = 60.0;   // how far (m, more over a coarse pixel) a view inside goes before its sun depth is taken again
const float CL_SUN_AGAIN_SEEN = 0.0;   // while more than this much of the view is left
const float CL_PIECE_MEAN = 0.5;   // a piece's forms read over this much of its length at least
const float CL_OPAQUE = 0.02;    // what is left of a view that ends it
const float CL_DETAIL_SEEN = 0.3;  // what must be left of a view for the detail to show in it
// (The layer's: CL_LAYER_FROM .. TO, CL_GLOBE_*, sky/scene_cloud_layer.glsl.)
// A far field's clumps lit as relief (cloudFieldScore over its threshold a
// height, CL_FIELD_RELIEF of the deck's thickness a unit of the score, its
// slope toward the sun read CL_FIELD_SUN_STEP footprints sunward), greyer
// toward their thin rims (CL_FIELD_RIM at the rim, all of it CL_FIELD_CORE
// of the score in), the whole CL_FIELD_KEEP so the field's mean holds.
const float CL_FIELD_RELIEF = 0.7;
const float CL_FIELD_SUN_STEP = 2.0;
const float CL_FIELD_LIT = 0.6;
const float CL_FIELD_RIM = 0.55;
const float CL_FIELD_CORE = 1.5;
const float CL_FIELD_KEEP = 1.6;

// What the march is doing.
// The sky's and the ground's light a cloud scatters, of their irradiance:
// the radiance a uniform sky gives (1/pi), not the irradiance itself, which
// lit a heap's shaded side and underside nearly as its sunlit top.
const float CL_AMBIENT = 1.0 / PI;
const int CL_TRACE = 0;  // through clear air, or across a skin
const int CL_HALVE = 1;   // halving toward a face
const int CL_INSIDE = 2;  // through a cloud

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

// The point as the cache sees it, the cache over a footprint and the forms
// there (clAt, clCache, clBaseShows, clFormsAt, ...): shared with the
// volume's bakes.
#include "sky/cloud_forms_at.glsl"

// How far (m) the view at `w` is sure to meet no cloud, from the cache's
// level `k`: its cell's own (the gap between the cell's box and the nearest
// that holds any), a little less for the chart's stretch.
float clFree(ClAt w, int k) {
    const int B = CL_CACHE_BINS;
    ivec2 c = ivec2(floor(w.xy / cloudCacheCell(k))) & (CL_CACHE_SIDE - 1);
    int b = clamp(int(floor(w.h * float(B))), 0, B - 1);
    return 0.98 * texelFetch(cloudCacheMap, ivec3(c, k * B + b), 0).w;
}

// How far (m) on the view at a point whose base reads `base` (with the
// margin of the base's change across a step) is sure to meet no cloud over
// footprint `foot`: where even the skin's most out leaves the surface
// short of it, by how far the base must still rise over how fast it can
// (the base is the least of one rising 1 over an edge's metres at most,
// any way, and one rising CLOUD_EDGE_STEEPEST up or down and little
// across, a flat base: so along the view the faster of the two; a share cut into clouds,
// cloudSurfaceBase, `q` of it, CLOUD_CUT_STEEPEST across), along a view
// `vs` (the sine of its slope) from the horizontal. Under 0 the world's
// base says how far the clear air is from cloud, so between heaps and
// over them this is most of the way there. (The base as the cache holds
// it: the lumps only carve it, cloudLumpsOf.)
float clBaseClear(float base, float foot, float q, float vs) {
    float w = cloudSurfaceBlur(foot);
    float most = base + (CL_BASE_MARGIN - 1.0) * abs(base) + CL_BASE_PAD + 0.55 * CLOUD_SKIN * cloudSkinAt(foot) * (1.0 - w) - 0.5 + 0.5 * w;
    float rate = max(1.0, vs * CLOUD_EDGE_STEEPEST + 0.2 * sqrt(max(1.0 - vs * vs, 0.0))) * mix(1.0, CLOUD_CUT_STEEPEST, q);
    return most < 0.0 ? -most * clouds.forms.w / rate : 0.0;
}

// The cloud's optical depth from a point of its face `a` (air space) toward
// the sun `s`, the view's footprint there `foot`: its own forms over the
// first few hundred metres (so a heap's sunward side is lit, its far side
// and underside shaded, and the skin's creases shadowed by the lobes about
// them, which the cache's cells are too coarse to tell), each piece read
// over its length; the cache's depth from where they end. Once a face, not
// marched.
const int CL_SUN_PIECES = 3;
float clSunDepth(vec3 a, vec3 s, float foot, bool detail) {
    float sc = max(1.0, foot / 15.0);
    float tau = 0.0, at = 0.0, len = 25.0 * sc;
    for (int i = 0; i < CL_SUN_PIECES; ++i) {
        ClAt w = clAt(a + s * (at + 0.5 * len));
        if (w.h >= 1.0) return tau;   // (out through the deck's top)
        float f = max(foot, 3.0 * len);
        tau += clFormsAt(w, clLod(w, f), f, foot, detail && i == 0).sigma * len;
        at += len;
        len *= 2.8;
    }
    ClAt w = clAt(a + s * at);
    if (w.h <= 0.0 || w.h >= 1.0) return tau;
    return tau + clCache(w, clLod(w, foot)).y;
}

// The globe's column at body point `p` over a pixel's footprint `foot` (m):
// where the pixel is wider than the globe's texel, the mean of four reads
// across it (a rotated grid), so texels finer than the pixel (a deck's
// cells, a field's clusters) average to their mean, never alias to grain.
vec4 clGlobeColumn(vec3 p, float foot) {
    vec3 up = normalize(p);
    float wide = smoothstep(1.0, 2.0, foot / cloudGlobeTexel());
    if (wide <= 0.0) return cloudGlobeColumn(up);
    vec3 e1 = normalize(cross(up, abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 e2 = cross(up, e1);
    float o = wide * foot / length(p);
    return 0.25 * (cloudGlobeColumn(normalize(up + o * (0.375 * e1 + 0.125 * e2))) +
                   cloudGlobeColumn(normalize(up + o * (-0.125 * e1 + 0.375 * e2))) +
                   cloudGlobeColumn(normalize(up + o * (-0.375 * e1 - 0.125 * e2))) +
                   cloudGlobeColumn(normalize(up + o * (0.125 * e1 - 0.375 * e2))));
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
// How much more or less sun an opaque top seen as a layer takes at `p`
// (air space) for the sun `s`, seen over `foot` metres: the top's lumps,
// the engine's noise CL_TOP_TILE edges a tile (its billows some
// CL_TOP_TILE / 8 edges across) CL_TOP_RELIEF edges high, as a surface
// whose slope toward the sun turns it to the sun or away (its cosine to the
// sun over a flat top's: the sunward side bright, the lee grey, lit by the
// sky alone where the slope hides it). A world that shapes its slabs'
// tops by the same noise at the same tile is lit as it is shaped; one that
// does not still has lumps of about the size overcast tops have. Faded out
// as the lumps come under CL_TOP_PIXELS pixels, where their mean is the
// flat top's.
const float CL_TOP_TILE = 16.0;
const float CL_TOP_RELIEF = 0.8;
const float CL_TOP_PIXELS = 5.0;
float clTopShade(vec3 p, vec3 s, float foot) {
    float T = CL_TOP_TILE * clouds.forms.w;
    float vis = smoothstep(0.4 * CL_TOP_PIXELS, CL_TOP_PIXELS, 0.125 * T / max(foot, 1.0));
    if (vis <= 0.0) return 1.0;
    vec3 up = normalize(p);
    float sinE = dot(up, s);
    vec3 st = s - up * sinE;
    float cosE = length(st);
    if (cosE < 1e-3) return 1.0;
    st /= cosE;
    float ds = max(foot, T / 32.0);
    float a = cloudNoise(cloudDrift(cloudToBody(p + st * ds)) / T, foot / T).x;
    float b = cloudNoise(cloudDrift(cloudToBody(p - st * ds)) / T, foot / T).x;
    float slope = CL_TOP_RELIEF * clouds.forms.w * (a - b) / (2.0 * ds);
    float lit = (sinE - slope * cosE) / sqrt(1.0 + slope * slope);
    return mix(1.0, clamp(lit / max(sinE, 0.08), 0.15, 2.5), vis);
}

void clIntegrate(inout ClMarch m,vec3 p, float tm, float dt, float sigma, vec2 tau, float about, vec3 s,
                 float nu, float ph[6], float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow, float down) {
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
    // (Six octaves, each's extinction 0.6 of the last's and its light 0.8:
    // a thick cloud seen from the sun's side sends back about what a white
    // diffuser would, as a deck from orbit does, while a heap's far side
    // and underside, deep from the sun, stay grey; at 0.4 the deep octaves
    // lit them nearly as its top.)
    float powder = mix(1.0 - exp(-about), 1.0, clamp(nu, 0.0, 1.0));
    float ms = 0.0, a = 1.0, b = 1.0;
    for (int o = 0; o < 6; ++o) {
        ms += b * ph[o] * exp(-tau.x * a) * (o == 0 ? mix(1.0, powder, 0.5) : powder);
        a *= 0.6;
        b *= 0.8;
    }
    // (The octaves past the sixth, b / (1 - 0.8) of the light, isotropic:
    // the deep body's diffuse reflection, which leaves through the face the
    // sun lights, so it reaches a view that looks down into the cloud, the
    // sun over it (a heap's top seen from above, which the six octaves'
    // forward lobes send back about half a white diffuser's light), and
    // never one that looks up at its base or across at its side.)
    ms += down * b / 0.2 * (0.25 / PI) * exp(-tau.x * a) * powder;
    // The sky's light, less what the cloud above hides (a floor for what
    // its body scatters about), and the ground's from below.
    float sees = mix(0.2, 1.0, exp(-0.15 * tau.y));
    vec3 ambient = (skyAbove * (0.35 + 0.65 * h) * sees + groundBelow * (0.6 - 0.4 * h)) * CL_AMBIENT;
    vec3 S = albedo * sigma * (E * sunAir * ms + ambient);
    float Tr = exp(-sigma * dt);
    m.L += m.T * (S - S * Tr) / sigma;
    float took = m.T * (1.0 - Tr);
    m.depthSum += took * tm;
    m.weightSum += took;
    m.T *= Tr;
}

#ifdef CL_DEBUG_VOLUME
#include "sky/cloud_volume_debug.glsl"
#endif

void main() {
#ifdef CL_DEBUG_VOLUME
    if (clouds.volE2.w > 1.5) {
        outColor = clVolSlices(int(clouds.volE2.w + 0.5));
        return;
    }
#endif
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
#ifdef CL_DEBUG_VOLUME
    if (clouds.volE2.w > 0.5) {
        // (The volume drawn instead of the forms: sky/cloud_volume_debug.glsl.)
        ClMarch mv = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
        clVolMarch(mv, ro, rd, spanA, spanB, s, E, nu, ph, albedo, pixel);
        float av = 1.0 - mv.T;
        if (av < 1.0 / 1024.0) discard;
        vec3 airT, airL;
        sceneAtmosphereAir(rd * (mv.depthSum / max(mv.weightSum, 1e-6)), airT, airL);
        outColor = vec4(min(mv.L * airT + av * airL, vec3(SCENE_HDR_MAX)), av);
        return;
    }
#endif

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
    clLumps = smoothstep(CL_LUMPS_FROM, CL_LUMPS_TO, abs(dot(rd, normalize(ro + rd * sp.x))));
    clDown = smoothstep(CL_DOWN_FROM, CL_DOWN_TO, -dot(rd, normalize(ro + rd * sp.x))) *
             smoothstep(clouds.shell.y, clouds.shell.y + CL_DOWN_OVER * (clouds.shell.y - clouds.shell.x), length(ro));
    float wl =length(ro) > clouds.shell.y ? smoothstep(CL_LAYER_FROM, CL_LAYER_TO, pixel * sp.x / thick) : 0.0;
    ClMarch ml = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    vec4 dbgLCol = vec4(0.0);
    float dbgLMu = 1.0;
    for (int i = 0; i < 2 && wl > 0.0; ++i) {
        vec2 q = i == 0 ? spanA : spanB;
        if (q.x >= q.y) continue;
        ClAt w = clAt(ro + rd * q.x);
        float lod = clLod(w, pixel * q.x);
        int k = int(lod);
        // (The cache's column where its cell is about as fine as the
        // globe's or finer, CL_GLOBE_FROM .. CL_GLOBE_TO of its texel; the
        // globe's where it is coarser, or where the pixel spans many of
        // the clouds, CL_GLOBE_CLOUDS_FROM .. TO edges, so it is the share
        // they cover, never the cache's cut of a share into clouds a few
        // of its cells wide.)
        vec2 lc = cloudLayerCell(lod, pixel * q.x);
        float footCell = lc.x, globe = lc.y;
        vec4 col = vec4(0.0);
        if (globe < 1.0) {
            col = cloudCacheColumn(w.xy, k);
            if (k + 1 < CL_CACHE_LEVELS) col = mix(col, cloudCacheColumn(w.xy, k + 1), fract(lod));
        }
        if (globe > 0.0) col = mix(col, clGlobeColumn(w.p, pixel * q.x), globe);
        float len = q.y - q.x;
        // (The cell's mean cut to the pixel: as much more or less as the
        // billows fill of the pixel's footprint than of the cell's, at the
        // base where the column sends its light from. Not the globe's: its
        // texel is as wide as the pixel or more, the billows' mean over
        // both the same.)
        float cut = 1.0;
        if (globe < 1.0) {
            ClAt wm = clAt(ro + rd * (q.x + len * (1.0 - col.z)));
            wm.h = col.z;
            float base = clBase(wm, lod);
            vec3 pd = cloudDrift(wm.p);
            float footPixel = pixel * q.x;
            float shareCell = cloudFormsShare(cloudBillows(pd, footCell), base, footCell);
            float sharePixel = cloudFormsShare(cloudBillows(pd, footPixel), base, footPixel);
            cut = mix(clamp(sharePixel / max(shareCell, 1e-3), 0.0, 4.0), 1.0, globe);
        }
        // The view's depth through the layer slanted `m` times its own: the
        // share of the view its clouds cover (col.x's over the depth within
        // them, col.w) as from above, and within them the depth the slant's.
        // (Not col.x's slanted: a field of opaque clouds over a share of the
        // column slanted to the horizon is no white sheet.)
        float m = len / thick;
        float cv = clamp((1.0 - exp(-col.x)) / max(1.0 - exp(-col.w), 1e-4), 0.0, 1.0);
        // (That share, of a cell far wider than the pixel, cut into clouds
        // at the pixel by the engine's field, cloudFieldShare: its footprint
        // the pixel's along the ground, stretched by the slant.)
        // Its clumps lit as relief: the score over the threshold their
        // height, its slope toward the sun from a second read sunward
        // (lit on the sun's side, shaded and self-shadowed on the far one),
        // greyer toward their thin rims.
        vec3 upq = normalize(ro + rd * q.x);
        float vs = abs(dot(rd, upq));
        float footV = pixel * q.x / max(vs, 0.25);
        float fieldShade = 1.0;
        float fade = cloudFieldFade(footCell, footV);
        if (fade > 0.0 && cv > 0.0 && cv < 1.0) {
            vec3 pd = cloudDrift(w.p);
            vec2 zv = cloudFieldScore(pd, clouds.forms.w, footV);
            float th = cloudFieldThreshold(cv);
            cv = mix(cv, cloudFieldCut(zv, th), fade);
            float sinE = dot(upq, s);
            vec3 st = s - upq * sinE;
            float cosE = length(st);
            float ds = CL_FIELD_SUN_STEP * footV;
            float z2 = cosE > 1e-3 ? cloudFieldScore(pd + cloudToBody(st / cosE) * ds, clouds.forms.w, footV).x : zv.x;
            float h = max(zv.x - th, 0.0);
            float slope = CL_FIELD_RELIEF * thick * (max(z2 - th, 0.0) - h) / ds;
            // (Its light over a flat top's, odd in the slope, so a field's
            // sun and lee sides average to the flat top's.)
            float lit = 1.0 + CL_FIELD_LIT * tanh(-slope * cosE / max(sinE, 0.15));
            float tone = mix(CL_FIELD_RIM, 1.0, smoothstep(0.0, CL_FIELD_CORE, h));
            fieldShade = mix(1.0, lit * tone * CL_FIELD_KEEP, fade);
        }
        float tauView = -log(max(1.0 - cv * (1.0 - exp(-col.w * m)), 1e-4)) * cut;
        if (tauView < 1e-4) continue;
        float at = q.x + len * clamp(col.y / max(col.x, 1e-4), 0.0, 1.0);
        vec3 pl = ro + rd * at;
        float mu = max(dot(normalize(pl), s), 0.1);
        if (!sky) {
            clSkyAt(ro, rd * at, skyAbove, groundBelow);
            sky = true;
        }
        // (The powder's depth the column's own: what it sends comes from
        // cloud as deep as it is, however thin the view's mean. An opaque
        // top's lumps lit from the side, clTopShade.)
        float shade = mix(1.0, clTopShade(pl, s, pixel * at), smoothstep(1.0, 3.0, tauView)) * fieldShade;
        clIntegrate(ml, pl, at, len, tauView / len, vec2(col.y / mu, col.y), col.w, s, nu, ph, albedo, E * shade,
                    skyAbove, groundBelow, 0.0);
        dbgLCol = col;
        dbgLMu = mu;
    }
#ifdef CL_DEBUG_LAYER
    {
        // The layer's terms: 1 the column's depth, 2 its alpha, 3 its
        // reflectance (of a white diffuser's), 4 that over its alpha.
        const int mode = CL_DEBUG_LAYER;
        float refl = dot(ml.L, vec3(0.2126, 0.7152, 0.0722)) / max(dot(E, vec3(0.2126, 0.7152, 0.0722)) * dbgLMu / PI, 1e-6);
        float v = mode == 1 ? dbgLCol.x : mode == 2 ? 1.0 - ml.T : mode == 3 ? refl : refl / max(1.0 - ml.T, 1e-3);
        float k = mode == 1 ? 1.0 : 0.25;
        // (black none, red < k/4, yellow < k/2, green < k, cyan < 2k, blue < 4k, white more)
        vec3 c = v <= 0.0 ? vec3(0) : v < 0.25 * k ? vec3(1, 0, 0) : v < 0.5 * k ? vec3(1, 1, 0) : v < k ? vec3(0, 1, 0)
               : v < 2.0 * k ? vec3(0, 1, 1) : v < 4.0 * k ? vec3(0, 0, 1) : vec3(1);
        outColor = vec4(c * 1e6, 1.0);
        return;
    }
#endif

    int mode = CL_TRACE;
    float t = sp.x;          // where the view is
    float probeEnd = t;      // the band says nothing more before here
    float xPrev = t;         // the furthest the view is known clear to
    float hLo = 0.0, hHi = 0.0;   // the face's bounds while it is halved
    int halvings = 0;
    float len = 0.0;         // inside: the next piece's length
    float clearAt = -1.0, clearFace = 0.0;   // the last clear sample, and the forms' field there
    bool clearLazy = false;                  // (that field not read yet)
    float fLo = 0.0, fHi = 0.0;             // the field at the face's bounds (fLo 0: not known)
    int side = 0;                            // which bound the last halving moved
    int faces = 0;           // faces halved for
    float tFace = -1.0, tauFace = 0.0, tauUp = 0.0;   // inside: where the sun's depth was taken, its depths toward the sun and up there
    float tauIn = 0.0;       // inside: the view's depth from tFace to the piece
    int steps = 0;
    int dbgFree = 0, dbgB = 0, dbgSkip = 0, dbgSkin = 0, dbgIn = 0, dbgHalve = 0, dbgPiece = 0;
    float dbgTau = -1.0;
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
                    if (!(bj.z < 1e-4 || max(r, r1) < Rlo || rmin > Rhi)) {
                        // (The view meets the band in this cell: crossed at
                        // once to where it does, from under it or over it.)
                        if (r < Rlo || r > Rhi) {
                            float R = r < Rlo ? Rlo : Rhi;
                            float bq = dot(ro, rd), dq = bq * bq - (dot(ro, ro) - R * R);
                            float tin = r < Rlo ? -bq + sqrt(max(dq, 0.0)) : -bq - sqrt(max(dq, 0.0));
                            if (dq > 0.0 && tin > t) skip = max(skip, min(tin, t + dj) - t);
                        }
                        break;
                    }
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
            // (this level's, or a coarser one's where that says more), and
            // the base's own: how far it must still rise before even the
            // skin's most out could reach it.
            float dFree = clFree(w, k);
            int kc = min(k + CL_FREE_LEVELS, CL_CACHE_LEVELS - 1);
            if (kc > k) dFree = max(dFree, clFree(w, kc));
            float dB = clBaseClear(clBase(w, lod), foot, clShareLike(lod), abs(dot(rd, normalize(a))));
            float skin = max(CL_SKIN_PIXELS * foot, 1.0);
            float d = max(max(dFree, dB), skip);
            if (d > skin) {
                if (d == dFree) ++dbgFree; else if (d == dB) ++dbgB; else ++dbgSkip;
                t += d;
                xPrev = t;
                continue;
            }
            // In a cloud's skin: the forms here, crossed a few pixels at a
            // time while they hold none. (Where the base's bound already
            // says the point is clear, dB > 0, the forms there hold none
            // whatever the skin does: their field is read only if the next
            // sample meets a face, for its false position.)
            if (dB > 0.0) {
                ++dbgSkin;
                clearAt = xPrev = t;
                clearLazy = true;
                t += skin;
                continue;
            }
            ClForms f = clFormsAt(w, lod, foot, foot, m.T > CL_DETAIL_SEEN);
            if (f.sigma <= 0.0) ++dbgSkin; else ++dbgIn;
            if (f.sigma <= 0.0) {
                clearAt = xPrev = t;
                clearFace = f.face;
                clearLazy = false;
                t += skin;
                continue;
            }
            // The view meets cloud: its face lies between the last clear
            // point and here.
            hLo = max(xPrev, sp.x);
            hHi = t;
            if (hLo == clearAt && clearLazy) {
                ClAt wc = clAt(ro + rd * clearAt);
                float fc = pixel * clearAt;
                clearFace = clFormsAt(wc, clLod(wc, fc), fc, fc, m.T > CL_DETAIL_SEEN).face;
                clearLazy = false;
            }
            fLo = hLo == clearAt ? clearFace : 0.0;
            fHi = f.face;
            halvings = faces < CL_FACES
                         ? clamp(int(ceil(log2(max((hHi - hLo) / (CL_FACE_PIXELS * foot), 1.0)))), 0, CL_FACE_HALVINGS)
                         : 0;
            ++faces;
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
            ++dbgHalve;
            float s = fLo < 0.0 ? clamp(fLo / (fLo - fHi), 0.1, 0.9) : 0.5;
            float x = mix(hLo, hHi, s);
            ClAt w = clAt(ro + rd * x);
            float foot = pixel * x;
            ClForms f = clFormsAt(w, clLod(w, foot), foot, foot, m.T > CL_DETAIL_SEEN);
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
            tFace = -1.0;
            continue;
        }
        ++dbgPiece;
        float L = min(len, sp.y - t);
        float x = t + 0.5 * L;
        ClAt w = clAt(ro + rd * x);
        float foot = pixel * x;
        float lod = clLod(w, foot);
        // (The forms' mean over the piece: it is taken as one, so what is
        // finer than it along the view is its mean, not a point of it that
        // the next pixel's piece misses.)
        ClForms f = clFormsAt(w, lod, max(foot, CL_PIECE_MEAN * L), foot, m.T > CL_DETAIL_SEEN);
        float sigma = f.sigma;
        if (sigma <= 0.0) {
            // Out of the cloud: traced on from here.
            clearAt = t = xPrev = x;
            clearFace = f.face;
            clearLazy = false;
            mode = CL_TRACE;
            tFace = -1.0;
            continue;
        }
        if (!sky) {
            clSkyAt(ro, rd * x, skyAbove, groundBelow);
            sky = true;
        }
        // The depth toward the sun: taken at the face, and again wherever the
        // view has gone CL_SUN_AGAIN on from where it was last taken; more
        // (less) between as the view goes on into the cloud away from
        // (toward) the sun, by the forms it has crossed since. (Taken once a
        // cloud and carried on by the piece's own forms times the whole way
        // in, a view that grazed a near lobe's rim lit the far lobe behind
        // it by that rim's depth, its neighbour, which missed the rim, by the
        // far lobe's own: the near lobe's outline stepped pixel by pixel.)
        if (tFace < 0.0) {
            tFace = t;
            tauFace = clSunDepth(ro + rd * t, s, pixel * t, m.T > CL_DETAIL_SEEN);
            tauUp = clCache(w, lod).z;
            tauIn = 0.0;
        } else if (m.T > CL_SUN_AGAIN_SEEN && x - tFace > CL_SUN_AGAIN * max(1.0, foot / 15.0)) {
            tFace = x;
            tauFace = clSunDepth(ro + rd * x, s, foot, m.T > CL_DETAIL_SEEN);
            tauUp = clCache(w, lod).z;
            tauIn = -0.5 * sigma * L;
        }
        vec2 tau = vec2(max(tauFace - (tauIn + 0.5 * sigma * L) * nu, 0.0), tauUp);
        tauIn += sigma * L;
        if (dbgTau < 0.0) dbgTau = tau.x;
        // (The powder's depth over the pixel's footprint, not the piece's
        // length, which grows from the face.)
        float down = clDown * smoothstep(0.0, 0.2, dot(s, normalize(ro + rd * x)));
        clIntegrate(m, ro + rd * x, x, L, sigma, tau, sigma * max(120.0, foot), s, nu, ph, albedo, E,
                    skyAbove, groundBelow, down);
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
#ifdef CL_DEBUG_KINDS
    {
        int most = max(max(dbgFree, dbgB), max(dbgSkin, dbgIn));
        vec3 c = most == 0 ? vec3(0.0) : most == dbgFree ? vec3(1, 0, 0) : most == dbgB ? vec3(0, 1, 0)
               : most == dbgSkin ? vec3(0, 0, 1) : vec3(1, 1, 1);
        outColor = vec4(c * 1e6, 1.0);
        return;
    }
#endif
#ifdef CL_DEBUG_TAU
    {
        // (black none, then red <1, yellow <3, green <10, cyan <30, blue <100, white more)
        vec3 c = dbgTau < 0.0 ? vec3(0) : dbgTau < 1.0 ? vec3(1, 0, 0) : dbgTau < 3.0 ? vec3(1, 1, 0)
               : dbgTau < 10.0 ? vec3(0, 1, 0) : dbgTau < 30.0 ? vec3(0, 1, 1) : dbgTau < 100.0 ? vec3(0, 0, 1) : vec3(1);
        outColor = vec4(c * 1e6, 1.0);
        return;
    }
#endif
#ifdef CL_DEBUG_COUNT
    steps = CL_DEBUG_COUNT;
#endif
#ifdef CL_DEBUG_STEPS
    int bin = steps == 0 ? 0 : steps < 12 ? 1 : steps < 24 ? 2 : steps < 36 ? 3 : steps < 48 ? 4 : steps < 72 ? 5
            : steps < 128 ? 6 : 7;
    // (The HDR target is in physical units: a bin's colour scaled up to show.)
    outColor = vec4(vec3(bin & 1, (bin >> 1) & 1, bin >> 2) * 1e6, 1.0);
    return;
#endif
#ifdef CL_DEBUG_OUT
    {
        float mu0 = max(dot(normalize(ro + rd * (m.depthSum / max(m.weightSum, 1e-6))), s), 0.1);
        float refl = dot(m.L, vec3(0.2126, 0.7152, 0.0722)) / max(dot(E, vec3(0.2126, 0.7152, 0.0722)) * mu0 / PI, 1e-6);
        float v = CL_DEBUG_OUT == 1 ? alpha : CL_DEBUG_OUT == 2 ? refl : refl / max(alpha, 1e-3);
        // (black none, red < .25, yellow < .5, green < .75, cyan < 1, blue < 1.5, white more)
        vec3 c = alpha <= 1e-3 ? vec3(0) : v < 0.25 ? vec3(1, 0, 0) : v < 0.5 ? vec3(1, 1, 0) : v < 0.75 ? vec3(0, 1, 0)
               : v < 1.0 ? vec3(0, 1, 1) : v < 1.5 ? vec3(0, 0, 1) : vec3(1);
        outColor = vec4(c * 1e6, 1.0);
        return;
    }
#endif
    if (alpha < 1.0 / 1024.0) discard;
    // The air between the eye and the deck, at its mean depth.
    vec3 airT, airL;
    sceneAtmosphereAir(rd * (m.depthSum / max(m.weightSum, 1e-6)), airT, airL);
    vec3 col = m.L * airT + alpha * airL;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha);
}
