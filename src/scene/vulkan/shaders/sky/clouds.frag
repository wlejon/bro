#version 450
// The cloud deck (vulkan/sky/pass_clouds.h; docs/scene-world-architecture.md
// §13 "volumes", §11 "going up"): each pixel's view marched through the
// deck's shell, from below, inside and above it the same way, up to the
// opaque surfaces' depth (a hill before a cloud hides it, a cloud before a
// hill hides the hill), and blended over what is drawn.
//
// The march reads the deck's cache about the eye's nadir (sky/clouds_layout.h):
// its extinction (its mean over a cell), the depths from there toward the
// sun and up, how far the nearest cloud is, and each column's band. The
// view crosses the deck in at most two spans (one from below or inside;
// from above, one or, past the deck's bottom without meeting the ground,
// two). Each step reads the cache at the level whose cell is as wide as
// the pixel there, or the finest whose window holds the step, and is at
// most three quarters of a cell across and of a bin up. The air: the bands
// of the columns about the view's next cells, from CL_PROBE_LEVELS levels
// coarser down, cross what holds no cloud (or lies above or below what
// does) a cell at a time; and a step the cache says is clear goes on by
// the distance to the nearest cloud. The cloud: where its cell is as fine
// as the pixel, the cache's; where the pixel is finer (the cells are about
// a sixtieth of the distance from the nadir, so for any view but straight
// down that is most of what is near), the world's module's at the pixel's
// footprint, a step that holds cloud cut into leaves CL_LEAF_PIXELS pixels
// long, at most CL_MODULE_EVALS a pixel (the cache's past them). Where the
// view enters cloud, the cache is halved for the face (else a deck's top
// shows the steps' grid as contours). No jitter: the picture is the same
// every frame. At most CL_VIEW_STEPS steps a pixel.
//
// The light, as the land's: the sun above the air (lighting.atm's
// illuminance) through the air to the sample (the transmittance table, the
// planet's shadow included) and through the deck toward the sun (the
// cache's depth), with a two-lobed phase and multiple scattering's six
// octaves (Wrenninge 2013), the deeper ones dimmed at a cloud's thin edge
// (the powder); and the sky's light at the deck (the atmosphere's SH:
// above, less what the cloud over the sample hides, and the lit ground
// below, by the height in the deck). The scattering integrated over each
// step (Hillaire 2016), and the air between the eye and the deck from the
// same tables the land's is (sceneAtmosphereAir), at the deck's mean depth
// along the view, so a far deck hazes into the limb.

layout(location = 0) in vec3 inWorldDir;
layout(location = 0) out vec4 outColor;

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;

vec4 cloudLandGrid(int c, vec3 up) { return vec4(0.0); }

#include "sky/cloud_module.glsl"

const int CL_VIEW_STEPS = 256;
const int CL_PROBE_LEVELS = 5;   // how much coarser the coarsest probe's level is
const int CL_LEAVES = 8;         // a near step's leaves, at most
const int CL_MODULE_EVALS = 48;  // the module's samples a view, at most
const float CL_LEAF_PIXELS = 4.0;// a leaf's length, in pixels up the screen
const float CL_OPAQUE = 0.02;    // what is left of a view that ends it

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

// The cache at air-space point `a`, level `k`.
vec4 clCache(vec3 a, int k) {
    float r = length(a);
    float h = (r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x);
    vec2 xy = cloudChartAt(cloudToBody(a) / r);
    return textureLod(cloudCacheMap, cloudCacheCoord(xy, h, k), 0.0);
}

// What the march carries along a view.
struct ClMarch {
    vec3 L;          // light scattered toward the eye
    float T;         // what is left of the view
    float depthSum;  // distance weighted by what each step took
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

// One piece's light, integrated over its length (Hillaire 2016): `tau` the
// deck's depths from the point lit toward the sun and up, `ph` the octaves'
// phases.
void clIntegrate(inout ClMarch m, vec3 p, float tm, float dt, float sigma, vec2 tau, vec3 s, float nu,
                 float ph[6], float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow) {
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
    // (Its depth over the step or the pixel's footprint, whichever is the
    // longer: a far sample's extinction is its clouds' mean with the sky
    // between, thin, though each cloud in it is as thick as near ones.)
    float powder = mix(1.0 - exp(-sigma * max(max(120.0, dt), clouds.params.y * tm)), 1.0, clamp(nu, 0.0, 1.0));
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
    float ph[6];
    for (int o = 0; o < 6; ++o) ph[o] = clPhase(nu, exp2(-float(o)));

    // The sky's light at the deck, above it and from the ground below:
    // where the view first meets a cloud, once it does.
    vec3 skyAbove = vec3(0.0), groundBelow = vec3(0.0);
    bool sky = false;

    ClMarch m = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    int steps = 0, evals = 0;
    for (int span = 0; span < 2; ++span) {
        vec2 sp = span == 0 ? spanA : spanB;
        float t = sp.x, probeEnd = t;
        int probeLevel = CL_CACHE_LEVELS;
        bool clear = true;   // the last piece held next to no cloud
        while (t < sp.y && steps < CL_VIEW_STEPS && m.T > CL_OPAQUE) {
            ++steps;
            vec3 a = ro + rd * t;
            float r = length(a);
            vec3 up = a / r;
            vec2 xy = cloudChartAt(cloudToBody(up));
            int k = cloudCacheLevel(xy, pixel * t);
            if (k >= CL_CACHE_LEVELS) break;   // (past the cache: nothing held)
            float vs = abs(dot(rd, up));
            float hs = sqrt(max(1.0 - vs * vs, 0.0));
            // The probes: the band of the columns about the view's crossing
            // of the next coarse cell (CL_PROBE_LEVELS coarser), then of the
            // next cell; where the view crosses it outside the band, it is
            // crossed in one step, and where it is below or above the band,
            // the view goes on from where it meets it.
            vec4 band = vec4(0.0);
            bool skip = false;
            int top = min(k + CL_PROBE_LEVELS, CL_CACHE_LEVELS - 1);
            if (t < probeEnd) top = max(min(top, probeLevel - 1), k);
            for (int kb = top; kb >= k && !skip; --kb) {
                float dth = min(0.5 * cloudCacheCell(kb) / max(hs, 1e-4), sp.y - t);
                band = cloudCacheBand(xy, kb);
                float Rlo = mix(clouds.shell.x, clouds.shell.y, band.x);
                float Rhi = mix(clouds.shell.x, clouds.shell.y, band.y);
                float r1 = length(ro + rd * (t + dth));
                float tp = -dot(ro, rd);
                float rmin = tp > t && tp < t + dth ? length(ro + rd * tp) : min(r, r1);
                if (band.z < 1e-4 || max(r, r1) < Rlo || rmin > Rhi) {
                    t += dth;
                    skip = true;
                } else if (kb > k) {
                    // (Cloud about this coarse cell: the levels under it are
                    // the probes until the view leaves it.)
                    if (t >= probeEnd || kb < probeLevel) {
                        probeLevel = kb;
                        probeEnd = t + dth;
                    }
                } else if (r < Rlo || r > Rhi) {
                    // (Where the view meets the band.)
                    float R = r < Rlo ? Rlo : Rhi;
                    float b = dot(ro, rd), dd = b * b - (dot(ro, ro) - R * R);
                    float tin = r < Rlo ? -b + sqrt(max(dd, 0.0)) : -b - sqrt(max(dd, 0.0));
                    if (tin > t && tin < t + dth) {
                        t = tin;
                    } else if (!(dd > 0.0 && tin > t)) {
                        t += dth;
                        skip = true;
                    }
                }
            }
            if (skip) {
                clear = true;
                continue;
            }
            // A step at most three quarters of a cell across and of a bin up.
            float dt = min(0.75 * min(cloudCacheCell(k) / max(hs, 1e-4), binH / max(vs, 1e-4)), sp.y - t);
            if (dt <= 0.0) break;
            float xm = t + 0.5 * dt;
            vec4 c = clCache(ro + rd * xm, k);
            if (c.x * dt < 1e-5) {
                // Clear: on past the air the cache says holds no cloud about
                // here (its distance to cloud, less a texel's reach: the
                // lookup is a blend of its texels').
                t += max(dt, 0.5 * dt + c.w - max(cloudCacheCell(k), binH));
                clear = true;
                continue;
            }
            // Near, the module's share (where the pixel is finer than the
            // finest cell): a step that holds cloud cut into leaves.
            float wNear = 1.0 - smoothstep(0.5, 1.0, pixel * xm / cloudCacheCell(k));
            bool fine = wNear > 0.0 && c.x * dt > 0.003 && evals < CL_MODULE_EVALS;
            int n = fine ? clamp(int(ceil(dt / max(CL_LEAF_PIXELS * pixel * xm, 2.0))), 1, CL_LEAVES) : 1;
            if (n > 1) {
                // (The module over the whole step first, its mean over half
                // of it: where that holds next to nothing, the step is
                // crossed as clear, not cut. Not on the cache's mean: a
                // coarse cell's, drawn along the step, is a smear.)
                float whole = cloudExtinction(ro + rd * xm, max(0.5 * dt, pixel * xm));
                ++evals;
                if (whole * dt < 0.005) {
                    n = 1;
                    fine = false;
                    c.x = 0.0;
                }
            }
            float d = dt / float(n);
            for (int i = 0; i < n && m.T > CL_OPAQUE; ++i) {
                float a0 = t + float(i) * d, x = a0 + 0.5 * d;
                vec4 cv = n == 1 ? c : clCache(ro + rd * x, k);
                float sigma = cv.x;
                if (fine) {
                    sigma = mix(cv.x, cloudExtinction(ro + rd * x, pixel * x), wNear);
                    ++evals;
                }
                bool entered = clear;
                clear = sigma * d < 0.01;
                if (sigma <= 0.0) continue;
                float lo = a0, len = d;
                if (entered && cv.x * d > 1e-3) {
                    // The view enters cloud: where, by halving the cache
                    // between the last sample (which held next to none, half
                    // a piece back) and this one, so a cloud's face moves
                    // with the cloud, not with the steps' grid (whose rings
                    // it would show where it crosses them: contours on a
                    // deck's top, streaks along a far base). (The cache's,
                    // not the module's: four of its samples at each entry
                    // cost more than the rest of the march.)
                    float hi = x;
                    lo = max(a0 - 0.5 * d, sp.x);
                    for (int b = 0; b < 4; ++b) {
                        float xb = 0.5 * (lo + hi);
                        if (clCache(ro + rd * xb, k).x >= 0.5 * cv.x) hi = xb;
                        else lo = xb;
                    }
                    lo = 0.5 * (lo + hi);
                    len = a0 + d - lo;
                }
                // (A piece's light a free path in when it is thicker than
                // that, else at its middle: what an opaque piece sends comes
                // from its near edge, not from wherever its middle fell.)
                float litAt = lo + 0.5 * len;
                vec2 tau = cv.yz;
                if (sigma * len > 1.0) {
                    // (The cache's depth is its bins' centres', blended: a
                    // free path under a dense top it reads as half a bin of
                    // the cloud. At most a free path, then what the cache
                    // holds a bin on toward the sun.)
                    litAt = lo + 1.0 / sigma;
                    vec3 pl = ro + rd * litAt;
                    tau = clCache(pl, k).yz;
                    vec3 sb = normalize(pl) * binH;
                    tau = min(tau, vec2(clCache(pl + s * binH, k).y, clCache(pl + sb, k).z) + 1.0);
                }
                if (!sky) {
                    clSkyAt(ro, rd * litAt, skyAbove, groundBelow);
                    sky = true;
                }
                clIntegrate(m, ro + rd * litAt, litAt, len, sigma, tau, s, nu, ph, albedo, E, skyAbove, groundBelow);
            }
            t += dt;
        }
    }
    float alpha = 1.0 - m.T;
#ifdef CL_DEBUG_STEPS
    // (Defined 1, the steps a pixel takes, 2 the module's samples, as hues:
    // < 4 blue, < 8 cyan, < 16 green, < 32 yellow, < 64 red, < 128 magenta,
    // more white.)
    const vec3 hue[7] = vec3[7](vec3(0, 0, 1), vec3(0, 1, 1), vec3(0, 1, 0), vec3(1, 1, 0), vec3(1, 0, 0),
                                vec3(1, 0, 1), vec3(1, 1, 1));
    int nDbg = CL_DEBUG_STEPS == 1 ? steps : evals;
    int bDbg = nDbg < 4 ? 0 : nDbg < 8 ? 1 : nDbg < 16 ? 2 : nDbg < 32 ? 3 : nDbg < 64 ? 4 : nDbg < 128 ? 5 : 6;
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
