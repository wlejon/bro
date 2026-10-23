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
// cumulus are not stepped over. A step a few pixels long (afar) is taken
// whole, its density straight between its ends; a longer one is halved
// where it holds a cloud, each piece first sampled at its own length, down
// to leaves CL_LEAF_PIXELS long (at most CL_SUB_LEVELS halvings), so a gap
// between clouds costs a few samples and a cloud's edge is where it is; the
// first thick leaf finds the cloud's front by halving. The march is one
// loop of one sample each, whatever it is for.
// No jitter, so the picture is the same every frame, and the density at
// each sample is the world's at its piece's length or the pixel's,
// whichever is the wider, so what is finer than either is its mean and no
// slice of it shows. At most CL_VIEW_STEPS samples a pixel.
//
// The light, as the land's: the sun above the air (lighting.atm's
// illuminance) through the air to the sample (the transmittance table, the
// planet's shadow included) and through the deck toward the sun (five
// samples, each three times as long as the last, so a tower's body shades
// its far side), with a two-lobed phase and multiple scattering's six
// octaves (Wrenninge 2013), the deeper ones dimmed at a cloud's thin edge
// (the powder); and the sky's light at the deck (the atmosphere's SH:
// above, less what the cloud over the sample hides, and the lit ground
// below, by the height in the deck). The deck's depths toward the sun and
// up are marched for the samples that send much of what the view sees (at
// most CL_LIGHTS a view), the rest taking the last march's. The scattering
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
const int CL_SUB_LEVELS = 4;   // halvings of a step, at most (16 leaves)
const float CL_LEAF_PIXELS = 4.0;// a leaf's length, in pixels up the screen
const int CL_LIGHTS = 3;  // samples a view marches the light for, at most
const float CL_OPAQUE = 0.02;  // what is left of a view that ends it

float clHg(float nu, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * nu, 1e-4), 1.5));
}
// The phase of water droplets, as two lobes: a strong forward one (the
// silver lining) and a weak backward one.
float clPhase(float nu, float k) { return mix(clHg(nu, 0.8 * k), clHg(nu, -0.3 * k), 0.2); }

// The deck's optical depth from air-space `p` toward `s` (the sun, or up):
// `n` samples, each three times as long as the one before, from `base`, so
// the light reaches through a towering cumulus (a few kilometres) as well
// as the wisp at its edge. Each sample is the density's mean over a third
// of its length (or the pixel's footprint), so the far ones are its
// cloud's mean and no small cloud flickers in and out of a shadow.
float clDepthToward(vec3 p, vec3 s, float base, float foot, int n) {
    float tau = 0.0, at = 0.0, len = base;
    for (int k = 0; k < n; ++k) {
        tau += cloudExtinction(p + s * (at + 0.5 * len), max(foot, len * 0.35)) * len;
        at += len;
        len *= 3.0;
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
    int lights;      // the samples whose light has been marched
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

// One sample's light, integrated over its step (Hillaire 2016).
void clIntegrate(inout ClMarch m, inout vec2 tau, vec3 p, float tm, float dt, float sigma, vec3 s, float nu,
                 float albedo, vec3 E, vec3 skyAbove, vec3 groundBelow) {
    float r = length(p);
    vec3 up = p / r;
    float h = clamp((r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x), 0.0, 1.0);
    // The sun at the sample: through the air (and the planet's shadow),
    // then through the deck; and the deck above it, what it hides of the
    // sky: marched while what the sample sends still shows (a third of the
    // view left), the last march's after.
    vec3 sunAir = atmSunTransmittance(r, dot(up, s));
    // (Its first sample as long as a few pixels are wide there, not as the
    // step: steps differ from pixel to pixel, and the light with them
    // would stripe.)
    float lf = clouds.params.y * tm;
    // (Marched for the samples that send much of what the view sees, at
    // most CL_LIGHTS of them; a veil's thin samples, and what lies deep
    // behind a cloud's front, take the last march's.)
    float sends = m.T * (1.0 - exp(-sigma * dt));
    if ((sends > 0.03 && m.T > 0.3 && m.lights < CL_LIGHTS) || tau.x < 0.0) {
        ++m.lights;
        float base = clamp(2.0 * lf, 25.0, 400.0);
        tau.x = max(sunAir.r, max(sunAir.g, sunAir.b)) > 1e-5 ? clDepthToward(p, s, base, lf, 5) : 0.0;
        tau.y = clDepthToward(p, up, 4.0 * base, lf, 2);
    } else {
        // (Deeper than what still shows much: the last depths, deepened by
        // what the view has crossed since, so the light falls on smoothly
        // and no line shows where the marches stop.)
        tau += vec2(sigma * dt);
    }
    // Multiple scattering's octaves (Wrenninge), the deeper ones dimmed
    // where the cloud is thin about the sample (the powder: little light
    // has scattered about at a cloud's edge, so seen with the sun behind
    // the eye its edges are darker than its body; toward the sun, the edge
    // is the bright silver lining).
    // (Six octaves, each's extinction 0.4 of the last's and its light 0.8:
    // a thick cloud seen from the sun's side sends back about what a white
    // diffuser would, as a deck from orbit does; four at 0.5 and 0.6 left
    // it grey.)
    // (Its depth over the step when the step is the longer: a far sample's
    // extinction is its clouds' mean with the sky between, thin, though
    // each cloud in it is as thick as near ones.)
    float powder = mix(1.0 - exp(-sigma * max(120.0, dt)), 1.0, clamp(nu, 0.0, 1.0));
    float ms = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int o = 0; o < 6; ++o) {
        ms += b * clPhase(nu, c) * exp(-tau.x * a) * (o == 0 ? mix(1.0, powder, 0.5) : powder);
        a *= 0.4;
        b *= 0.8;
        c *= 0.5;
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

    ClMarch m = ClMarch(vec3(0.0), 1.0, 0.0, 0.0, 0);
    vec2 tau = vec2(-1.0);   // the last sample's depth toward the sun and up (none yet)
    int dbgProbe = 0, dbgSub = 0, steps = 0;

    // The march as one loop of one density sample each, whatever the
    // sample is for (a short step's end, a piece of a long one, a halving
    // of a cloud's front), so the pixels of a group, each at its own part
    // of its march, sample together rather than in turn.
    const int M_STEP_A = 1, M_STEP_B = 2, M_PIECE = 3, M_HALVE = 4, M_FRONT = 5;
    vec2 sp = hasA ? spanA : spanB;
    bool second = !hasA;     // on the view's last span
    float cap = (sp.y - sp.x) / float(CL_SPAN_STEPS);
    float t = sp.x, end = t, dt = 0.0;
    float edgeT = -1.0, edgeS = 0.0;   // the last short step's end and its density
    float sA = 0.0;
    int mode = 0;            // 0: a step to begin at t
    int levels = 0, level = 0, idx = 0;
    float d = 0.0;           // a long step's leaves' length
    float lo = 0.0, hi = 0.0, firm = 0.0, leafSigma = 0.0, leafA = 0.0, leafFoot = 0.0;
    int halves = 0;
    for (int it = 0; it < CL_VIEW_STEPS && m.T > CL_OPAQUE; ++it) {
        if (mode == 0) {
            if (t >= sp.y) {
                if (second || !hasB) break;
                second = true;
                sp = spanB;
                cap = (sp.y - sp.x) / float(CL_SPAN_STEPS);
                t = sp.x;
                edgeT = -1.0;
            }
            end = min(min(clGridNext(t, minStep, share), t + cap), sp.y);
            dt = end - t;
            float sub = max(CL_LEAF_PIXELS * pixel * t, 8.0);
            if (dt <= 2.0 * sub) {
                // A step as short as a few pixels are wide (afar, and the
                // deck crossed from above): the density at its ends, taken
                // as straight between them, so what it holds and where it
                // begins move smoothly as a cloud's top rises through it
                // (sampled at its middle, a deck's tops would show the
                // steps' heights as contours). Its end is the next one's
                // start.
                mode = edgeT == t ? M_STEP_B : M_STEP_A;
                sA = edgeS;
            } else {
                // A longer step: halved where it holds a cloud, down to
                // pieces a few pixels long (its leaves), each piece first
                // sampled once at its own length; what holds next to
                // nothing is crossed (its trace, the mean of clouds about it
                // a piece wide, would gather along a long view into veils).
                // So a step beside a cloud, or through a gap between clouds,
                // costs a few samples, and one through a cloud as many
                // leaves as reach its opaque front. (In order, without a
                // stack: piece `idx` of level `level`.)
                levels = min(int(ceil(log2(dt / sub))), CL_SUB_LEVELS);
                d = dt / float(1 << levels);
                level = 0;
                idx = 0;
                mode = M_PIECE;
                edgeT = -1.0;
            }
        }
        // The sample.
        float x, foot, len = 0.0, a = 0.0;
        if (mode == M_STEP_A) {
            x = t;
            foot = max(0.3 * dt, pixel * t);
        } else if (mode == M_STEP_B) {
            x = end;
            foot = max(0.3 * dt, pixel * end);
        } else if (mode == M_PIECE) {
            len = dt / float(1 << level);
            a = t + float(idx) * len;
            x = a + 0.5 * len;
            // (A leaf's across the view the pixel's footprint, widened only
            // by a third of its length, so a cloud's edge is as sharp as the
            // pixel and still no slice of it shows.)
            foot = level < levels ? max(len, pixel * x) : max(0.3 * d, pixel * x);
        } else if (mode == M_HALVE) {
            x = 0.5 * (lo + hi);
            foot = leafFoot;
        } else {
            x = 0.5 * (lo + leafA + d);
            foot = leafFoot;
        }
        float sigma = cloudExtinction(ro + rd * x, foot);
        ++steps;

        bool advance = false;   // the piece done: on to the next
        // What the sample lights, if anything: the view from litT over
        // litLen at litSigma, lit at litAt.
        float litSigma = 0.0, litLen = 0.0, litAt = 0.0;
        if (mode == M_STEP_A) {
            sA = sigma;
            mode = M_STEP_B;
            continue;
        } else if (mode == M_STEP_B) {
            float sB = sigma;
            edgeT = end;
            edgeS = sB;
            if (sA + sB > 0.0) {
                // Lit where the view has crossed half of what it meets in
                // the step, or half an optical depth when it holds more (its
                // front then): sigma(x) = sA + (sB - sA) x / dt.
                float sm = 0.5 * (sA + sB);
                float goal = 0.5 * min(sm * dt, 1.0);
                float k = 0.5 * (sB - sA) / dt;
                float xs = abs(k) * goal < 1e-4 * sA * sA ? goal / max(sA, 1e-9)
                                                          : (sqrt(max(sA * sA + 4.0 * k * goal, 0.0)) - sA) / (2.0 * k);
                litSigma = sm;
                litLen = dt;
                litAt = t + clamp(xs, 0.0, dt);
            }
            t = end;
            mode = 0;
        } else if (mode == M_PIECE) {
            if (level < levels) {
                ++dbgProbe;
                if (sigma * len >= 0.005) {
                    ++level;
                    idx *= 2;
                    continue;
                }
                advance = true;
            } else {
                ++dbgSub;
                firm = sigma > 0.0 ? smoothstep(0.5, 2.0, sigma * d) * smoothstep(0.5, 0.9, m.T) : 0.0;
                if (firm > 0.0) {
                    // The view's first thick leaf: where in it the cloud
                    // begins, by halving (else a deck seen from above shows
                    // its top in contours where the leaves cross it), eased
                    // in by how thick the leaf is and how little the view
                    // has met before it, so no line shows where it starts.
                    lo = a;
                    hi = a + d;
                    leafA = a;
                    leafSigma = sigma;
                    leafFoot = foot;
                    halves = 0;
                    mode = M_HALVE;
                    continue;
                }
                litSigma = sigma;
                litLen = d;
                litAt = a + min(0.5 * d, 1.0 / max(sigma, 1e-9));
                advance = true;
            }
        } else if (mode == M_HALVE) {
            if (sigma * d > 0.5) hi = x;
            else lo = x;
            if (++halves == 4) {
                lo = mix(leafA, lo, firm);
                mode = M_FRONT;
            }
            continue;
        } else {
            litSigma = mix(leafSigma, max(sigma, leafSigma), firm);
            litLen = leafA + d - lo;
            litAt = lo + min(0.5 * litLen, 1.0 / litSigma);
            mode = M_PIECE;
            advance = true;
        }
        // (A leaf's light a free path in, or at its middle when thinner:
        // what an opaque piece sends comes from its near edge, not from
        // wherever its middle fell, which would light a deck's top darker or
        // lighter row by row as the pieces cross it.)
        if (litSigma > 0.0) {
            if (!sky) {
                clSkyAt(ro, rd * litAt, skyAbove, groundBelow);
                sky = true;
            }
            clIntegrate(m, tau, ro + rd * litAt, litAt, litLen, litSigma, s, nu, albedo, E, skyAbove, groundBelow);
        }
        if (advance) {
            ++idx;
            while (level > 0 && (idx & 1) == 0) {
                idx >>= 1;
                --level;
            }
            if (level == 0 && idx > 0) {
                t = end;
                mode = 0;
            }
        }
    }
    float alpha = 1.0 - m.T;
#ifdef CL_DEBUG_STEPS
    outColor = vec4(vec3(float(dbgProbe) / 40.0, float(dbgSub) / 40.0, float(steps) / 160.0) * 4e4, 1.0);
    return;
#endif
    if (alpha < 1.0 / 1024.0) discard;
    // The air between the eye and the deck, at its mean depth.
    vec3 airT, airL;
    sceneAtmosphereAir(rd * (m.depthSum / max(m.weightSum, 1e-6)), airT, airL);
    vec3 col = m.L * airT + alpha * airL;
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), alpha);
}
