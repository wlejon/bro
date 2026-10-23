#version 450
// The cloud deck (vulkan/sky/pass_clouds.h; docs/scene-world-architecture.md
// §13 "volumes", §11 "going up"): each pixel's view marched through the
// deck's shell, from below, inside and above it the same way, up to the
// opaque surfaces' depth (a hill before a cloud hides it, a cloud before a
// hill hides the hill), and blended over what is drawn.
//
// The march. The view crosses the deck in at most two spans (one from below
// or inside; from above, one or, past the deck's bottom without meeting the
// ground, two). Its steps lie on one grid of distances from the eye, the
// same for every pixel (the shortest step out to where a share of the
// distance is longer, that share beyond), so neighbouring pixels sample a
// cloud at the same depths and an edge-on cloud is not cut into slabs that
// stripe from row to row; a crossing shorter than CL_SPAN_STEPS grid steps
// (the deck seen from above) is cut into that many instead, so its low
// cumulus are not stepped over. A step is first sampled once, at its
// footprint; where it meets a cloud, it and the steps after it are sampled
// again in sub-steps four pixels long (at most CL_SUB_STEPS a step), so a
// cloud's edge is where it is.
// No jitter, so the picture is the same every frame, and the density at
// each sample is the world's at its step's length or the pixel's,
// whichever is the wider, so what is finer than either is its mean and no
// slice of it shows. At most CL_VIEW_STEPS samples a pixel.
//
// The light, as the land's: the sun above the air (lighting.atm's
// illuminance) through the air to the sample (the transmittance table, the
// planet's shadow included) and through the deck toward the sun (two
// samples a sample, ever farther and wider), with a forward-scattering
// phase and multiple scattering's four octaves (Wrenninge 2013: each
// octave's extinction and phase's lobe halved, its light 0.6 of the one
// before); and the sky's light at the deck (the atmosphere's SH: above,
// and the lit ground below, by the height in the deck). The scattering
// integrated over each step (Hillaire 2016), and the air between the eye
// and the deck from the same tables the land's is (sceneAtmosphereAir), at
// the deck's mean depth along the view, so a far deck hazes into the limb.

layout(location = 0) in vec3 inWorldDir;
layout(location = 0) out vec4 outColor;

#include "common/scene_camera.glsl"
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
layout(set = 2, binding = CL_B_DEPTH) uniform sampler2D cloudDepth;

vec4 cloudLandGrid(int c, vec3 up) { return vec4(0.0); }

#include "sky/cloud_module.glsl"

const int CL_VIEW_STEPS = 160;
const int CL_SPAN_STEPS = 12;
const int CL_SUB_STEPS = 6;    // sub-steps a step, at most
const float CL_OPAQUE = 0.02;  // what is left of a view that ends it

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

// The deck's optical depth toward the sun from air-space `p`: two samples,
// the second eight times as far and as wide as the first.
float clSunDepth(vec3 p, vec3 s, float base, float foot) {
    float tau = 0.0, at = 0.0, len = base;
    for (int k = 0; k < 2; ++k) {
        tau += cloudExtinction(p + s * (at + 0.5 * len), max(foot, len)) * len;
        at += len;
        len *= 8.0;
    }
    return tau;
}

// The step grid's next distance past `t`: every `minStep` out to
// minStep / share, then each a share farther than the one before.
float clGridNext(float t, float minStep, float share) {
    float tc = minStep / share;
    if (t < tc) return (floor(t / minStep) + 1.0) * minStep;
    float lk = log(1.0 + share);
    float k = floor(log(t / tc) / lk) + 1.0;
    float g = tc * exp(k * lk);
    return g > t * (1.0 + 1e-4) ? g : tc * exp((k + 1.0) * lk);
}

// What the march carries along a view.
struct ClMarch {
    vec3 L;          // light scattered toward the eye
    float T;         // what is left of the view
    float depthSum;  // distance weighted by what each step took
    float weightSum;
};

// One sample's light, integrated over its step (Hillaire 2016).
void clIntegrate(inout ClMarch m, inout float tau, vec3 p, float tm, float dt, float sigma, vec3 s, float nu,
                 float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow) {
    float r = length(p);
    vec3 up = p / r;
    float h = clamp((r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x), 0.0, 1.0);
    // The sun at the sample: through the air (and the planet's shadow),
    // then through the deck: marched while what the sample sends still
    // shows (a third of the view left), the last march's after.
    vec3 sunAir = atmSunTransmittance(r, dot(up, s));
    // (Its first sample as long as a few pixels are wide there, not as the
    // step: steps differ from pixel to pixel, and the light with them
    // would stripe.)
    float lf = clouds.params.y * tm;
    if (m.T > 0.35 || tau < 0.0)
        tau = max(sunAir.r, max(sunAir.g, sunAir.b)) > 1e-5 ? clSunDepth(p, s, clamp(4.0 * lf, 40.0, 1000.0), lf) : 0.0;
    float ms = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int o = 0; o < 4; ++o) {
        ms += b * clPhase(nu, c) * exp(-tau * a);
        a *= 0.5;
        b *= 0.6;
        c *= 0.5;
    }
    vec3 ambient = skyAbove * (0.35 + 0.65 * h) + groundBelow * (0.6 - 0.4 * h);
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
    bool hasA = spanA.x < spanA.y, hasB = spanB.x < spanB.y;
    if (!hasA && !hasB) discard;

    vec3 s = lighting.atm.sunDir.xyz;
    vec3 E = lighting.atm.sunIlluminance.rgb;
    float nu = dot(rd, s);
    float albedo = clouds.params.x;
    float pixel = clouds.params.y;
    float minStep = clouds.params.z, share = clouds.params.w;

    // The sky's light at the deck, above it and from the ground below:
    // where the view first meets a cloud, once it does.
    vec3 skyAbove = vec3(0.0), groundBelow = vec3(0.0);
    bool sky = false;

    ClMarch m = ClMarch(vec3(0.0), 1.0, 0.0, 0.0);
    int steps = 0;
    float tau = -1.0;   // the last sample's depth toward the sun (none yet)
    for (int span = 0; span < 2; ++span) {
        vec2 sp = span == 0 ? spanA : spanB;
        if (sp.x >= sp.y) continue;
        float cap = (sp.y - sp.x) / float(CL_SPAN_STEPS);
        float t = sp.x;
        bool dense = false;   // the step before met a cloud
        while (t < sp.y && steps < CL_VIEW_STEPS && m.T > CL_OPAQUE) {
            float end = min(min(clGridNext(t, minStep, share), t + cap), sp.y);
            float dt = end - t;
            float sub = max(4.0 * pixel * t, 8.0);
            int n = 1;
            if (dt > 2.0 * sub) {
                if (!dense) {
                    // One sample first: is there a cloud in this step?
                    float tm = t + 0.5 * dt;
                    vec3 p = ro + rd * tm;
                    float foot = max(dt, pixel * tm);
                    float sigma = cloudExtinction(p, foot);
                    ++steps;
                    // (A step that holds next to nothing is taken as
                    // clear: its trace, the mean of clouds about it a step
                    // wide, would gather along a long view into veils.)
                    if (sigma * dt < 0.005) {
                        t = end;
                        continue;
                    }
                }
                n = min(int(ceil(dt / sub)), CL_SUB_STEPS);
            }
            float d = dt / float(n);
            bool met = false;
            for (int i = 0; i < n && m.T > CL_OPAQUE; ++i) {
                float tm = t + (float(i) + 0.5) * d;
                vec3 p = ro + rd * tm;
                float foot = max(d, pixel * tm);
                float sigma = cloudExtinction(p, foot);
                ++steps;
                if (sigma <= 0.0) continue;
                met = true;
                float a = tm - 0.5 * d, len = d;
                if (m.T > 0.9 && sigma * d > 2.0) {
                    // The view's first opaque step: where in it the cloud
                    // begins, by halving (else a deck seen from above shows
                    // its top in contours where the steps cross it).
                    float lo = a, hi = a + d;
                    for (int k = 0; k < 4; ++k) {
                        float c = 0.5 * (lo + hi);
                        if (cloudExtinction(ro + rd * c, foot) * d > 0.5) hi = c;
                        else lo = c;
                    }
                    steps += 4;
                    len = a + d - lo;
                    a = lo;
                    sigma = max(cloudExtinction(ro + rd * (a + 0.5 * len), foot), sigma);
                }
                // Lit a free path in (or at the step's middle, when thinner):
                // what an opaque step sends comes from its near edge, not
                // from wherever its middle fell, which would light a deck's
                // top darker or lighter row by row as the steps cross it.
                tm = a + min(0.5 * len, 1.0 / sigma);
                if (!sky) {
                    vec3 at = rd * tm;
                    vec3 atUp = normalize(ro + at);
                    vec3 shc[6];
                    mat3 shFrame;
                    atmSkyLightSh(at, shc, shFrame);
                    skyAbove = atmShIrradiance(shc, shFrame, atUp);
                    groundBelow = atmShIrradiance(shc, shFrame, -atUp);
                    sky = true;
                }
                clIntegrate(m, tau, ro + rd * tm, tm, len, sigma, s, nu, albedo, E, skyAbove, groundBelow);
            }
            dense = met && n > 1;
            t = end;
        }
    }
    float alpha = 1.0 - m.T;
    if (alpha < 1.0 / 1024.0) discard;
    // The air between the eye and the deck, at its mean depth.
    vec3 airT, airL;
    sceneAtmosphereAir(rd * (m.depthSum / max(m.weightSum, 1e-6)), airT, airL);
    vec3 col = m.L * airT + alpha * airL;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha);
}
