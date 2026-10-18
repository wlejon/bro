// Vegetation: plants moving in the scene's wind field the way wood and
// leaves move, and the colour and light that make a crown one form. Needs
// scene_camera.glsl (the wind field and the clock).
//
// Every vertex (or instance, or leaf expanded from a segment record) carries
// four vec4s in node space, the layout bro.flora's emitters write
// (broflora/sway.h, docs/scene-nodes-api.js "Vegetation",
// bromesh::MeshData::vegetation):
//
//   v0 (root.xyz, height)        the plant's base and height
//   v1 (crown.xyz, crownRadius)  the leafy volume; radius 0: no bulk normal;
//                                negative: a wood segment record, (tip.xyz,
//                                -(1 + the weight at the tip))
//   v2 (anchor.xyz, weight)      where a leaf hangs (a bark vertex's point on
//                                its skeleton); the weight runs 0 where a limb
//                                leaves the trunk .. 1 at its twig tips
//   v3 (axis, twig, limb, trunk) packed words (integers held in floats): the
//                                leaf's stem direction (octahedral; 0 on
//                                wood); the limb's phase and its twigs'
//                                cantilever; the limb's cantilever; the trunk's
//
// The model. A plant is three cantilevers, each bending in the wind like a
// beam fixed at its base: the trunk (root to height), each limb (where it
// leaves the trunk to its farthest tip) and the limb's twigs (its terminal
// leafy sprays). broflora gives each its natural frequency (a limb's or a
// spray's tip stiffness 3 E I / L^3 over the mass its mode moves:
// Euler-Bernoulli's f ~ d / L^2 for a bare beam, slower for what it
// carries; the trunk's by Rayleigh over its stem's own bend) and its static
// tip deflection over its length at 10 m/s (a limb's from the drag on the
// leaves it carries over that stiffness; the trunk's from its stem, strained
// alike along its taper, so a slender stem leans further and its slim top
// bends most, in the shape h^p the trunk word carries). Here each level is
// driven by the drag, (U / 10 m/s)^1.3 (a crown streamlines, so drag grows
// slower than U^2), and answers as wood in leaf does (vegTreeLevel): it
// holds a lean downwind under the drag a quarter period back (a heavy
// level lags the gusts that reach it), damped by its size (vegDamping: a
// trunk rings weakly, a bough is near critically damped, a spray
// over-damped), buffeted about that lean by the eddies too small for the
// gust field (limbs and sprays; a trunk averages them over its crown), and
// ringing round its own frequency only as much as its damping lets. The
// trunk feels the wind at the root (it sums the crown's), averaged over
// its last half period (a low pass); a limb and its twigs feel it where
// they are, so a gust sweeps through a crown limb by limb. Three reads of
// the travelling gust field a vertex (the trunk's two, the limb's). Every
// motion is smooth in time to its second derivative (the gust field's
// quintic noise and hyperbolic threshold, vegSmoothAbs) and every
// frequency is fixed (scene_camera.glsl's oscillator clock). Movers
// (vegMoverPush) part the limbs and twigs within their reach. A limb's or
// a spray's bend has the shape of a loaded cantilever, s^2 (3 - s) / 2
// along it, so the bend is at its outer end, and the levels add down the
// hierarchy.
//
// Two layers, so a level-of-detail chain can share one pose:
//   vegPose   the trunk, limb and twig bend at a rest point: the offset and
//             the rotation of the wood there. Every tier (leaves, clump
//             cards, clump lobes) calls it with the same per-plant data
//             (VegPlant: root, height, trunk cantilever) and per-limb data
//             (VegLimb: limb and twig cantilevers, phase) and the same rest
//             point and weight, and gets the same pose, so crossfades between
//             tiers do not swim.
//   vegLeaf   the near tier's own leaf motion on top: the leaf turns with the
//             wood it hangs from (normals and shading follow the bend), flutters
//             about its petiole at a few hertz on its own phase, and in gusts
//             turns downwind and folds along its midrib.

const float VEG_REFERENCE_WIND = 10.0;        // m/s: broflora's kSwayReferenceWind
const vec2 VEG_FREQ_RANGE = vec2(0.05, 50.0);  // broflora's kSwayFreqRange, kSwaySwayRange, kSwayLengthRange
const vec2 VEG_SWAY_RANGE = vec2(1.0e-5, 1.0);
const vec2 VEG_LENGTH_RANGE = vec2(0.005, 200.0);
const vec2 VEG_SHAPE_RANGE = vec2(1.0, 4.0);     // broflora's kSwayShapeRange
const float VEG_TWO_PI = 6.28318530718;

// The ringing of a herb's stalk (vegLevel; trees take vegTreeLevel below).
// VEG_TURBULENCE: unresolved turbulence of 15% of the drag above 0.1 Hz
// with a -5/3 spectrum, whose narrow-band resonant RMS is sigma sqrt(pi /
// (4 * 6.76)) times the mean drag at 1 Hz and damping 1. VEG_GUST_KICK: the
// share of a gust's change that rings.
const float VEG_TURBULENCE = 0.051;
const float VEG_GUST_KICK = 0.4;
// A leaf's flutter about its petiole: lowest frequency and spread (Hz).
const vec2 VEG_LEAF_FLUTTER_HZ = vec2(2.0, 2.5);
// A leaf the eye passes through (a camera in a crown) shrinks to its middle
// within VEG_NEAR_SHRINK.y of the eye, gone by .x (m), rather than filling
// the view for a frame or two: at a few metres a second a leaf a few
// centimetres off is a different picture every frame. Colour passes only
// (a shadow caster defines VEG_SHADOW_CASTER).
const vec2 VEG_NEAR_SHRINK = vec2(0.25, 1.0);

// How much a limb and a twig rise and fall as they ring, against their
// sway (vegLevel's `bob`). A broadleaf's; a forest node sets its own
// (scene_forest.glsl's forestApplyLook: a conifer's long boughs bounce).
float vegLimbBob = 0.6;
float vegTwigBob = 0.8;

// A cantilever: natural frequency (Hz), tip deflection over length at
// VEG_REFERENCE_WIND (0: it does not bend), length (world metres).
struct VegCantilever { float freq; float sway; float len; };

// What every vertex of a plant shares: its root (camera-relative) and
// height (world), its trunk, and a hash of where it stands.
// `shape`: the trunk's bend, h^shape over its height (0: the uniform beam's).
struct VegPlant { vec3 root; float height; VegCantilever trunk; float seed; float shape; };

// What every vertex of a limb shares: the limb and its twigs, its phase.
struct VegLimb { VegCantilever limb; VegCantilever twig; float phase; };

// The wind at a plant: downwind and across (horizontal, unit); the drag
// (U / 10 m/s)^1.3 that drives its trunk (at its root, about a quarter of
// the trunk's period back: a heavy trunk lags the gusts), how much that
// drag changed over the trunk's last few tenths of a period (what sets a
// level ringing; a smooth |change|), and the clock.
struct VegWind { vec3 dir; vec3 side; float trunk; float kick; float t; };

// The wood's pose at a point: the offset it moves by and its rotation (a
// rotation vector: axis times radians), and the drag where it is (its
// limb's: what a leaf hanging there flutters in).
struct VegPose { vec3 offset; vec3 omega; float drag; };

// A packed word's number: log-quantised over `range`, code 0 is zero.
float vegLogValue(uint code, uint bits, vec2 range) {
    if (code == 0u) return 0.0;
    float t = float(code - 1u) / float((1u << bits) - 2u);
    return range.x * exp(t * log(range.y / range.x));
}

// The node's uniform scale: a plant drawn larger is a larger plant, so its
// lengths grow with it and its frequencies fall (f ~ d / L^2).
float vegScale(mat4 model) { return max(length(mat3(model)[1]), 1e-6); }

VegPlant vegPlant(mat4 model, vec4 v0, vec4 v3) {
    float scale = vegScale(model);
    VegPlant p;
    p.root = (model * vec4(v0.xyz, 1.0)).xyz;
    p.height = max(v0.w * scale, 0.05);
    uint w = uint(max(v3.w, 0.0) + 0.5);
    // broflora's packSwayTrunk: freq, sway (9 bits each), shape (6 bits, 0 unset).
    p.trunk = VegCantilever(vegLogValue(w >> 15, 9u, VEG_FREQ_RANGE) / scale,
                            vegLogValue((w >> 6) & 511u, 9u, VEG_SWAY_RANGE), p.height);
    uint sh = w & 63u;
    p.shape = sh == 0u ? 0.0 : VEG_SHAPE_RANGE.x + (VEG_SHAPE_RANGE.y - VEG_SHAPE_RANGE.x) * float(sh - 1u) / 62.0;
    vec3 rootAbs = p.root + camera.eyeWorld.xyz;
    p.seed = sceneWindHash(ivec2(floor(rootAbs.xz * 8.0)));
    return p;
}

VegLimb vegLimb(mat4 model, vec4 v3) {
    float scale = vegScale(model);
    uint a = uint(max(v3.y, 0.0) + 0.5), b = uint(max(v3.z, 0.0) + 0.5);
    VegLimb l;
    l.phase = float(a >> 18) / 64.0;
    l.twig = VegCantilever(vegLogValue((a >> 12) & 63u, 6u, VEG_FREQ_RANGE) / scale,
                           vegLogValue((a >> 6) & 63u, 6u, VEG_SWAY_RANGE),
                           vegLogValue(a & 63u, 6u, VEG_LENGTH_RANGE) * scale);
    l.limb = VegCantilever(vegLogValue(b >> 16, 8u, VEG_FREQ_RANGE) / scale,
                           vegLogValue((b >> 8) & 255u, 8u, VEG_SWAY_RANGE),
                           vegLogValue(b & 255u, 8u, VEG_LENGTH_RANGE) * scale);
    return l;
}

// A leaf's stem direction from its packed word (octahedral, 12 bits a
// component, plus one), node space.
vec3 vegUnpackAxis(float word) {
    uint u = uint(word + 0.5) - 1u;
    vec2 e = vec2(float(u >> 12), float(u & 4095u)) * (2.0 / 4095.0) - 1.0;
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

float vegPressure(float u) { return pow(max(u, 0.0) / VEG_REFERENCE_WIND, 1.3); }

// The drag at absolute world xz at wind-clock time t.
float vegDragAt(vec2 xz, float t, bool fine) {
    return vegPressure(camera.windField.z + 4.0 * sceneWindGustAtTime(xz, t, fine));
}

// |x|, rounded at 0 so what it drives has no kink where x changes sign.
float vegSmoothAbs(float x) { return sqrt(x * x + 0.0025) - 0.05; }

// A level's lag behind the drag it feels: a quarter of its period.
float vegLag(float freq) { return min(0.25 / max(freq, 0.05), 1.3); }

// The wind at absolute xz: the drag `lag` seconds back and its change over
// the `span` before that.
VegWind vegWindLagged(vec2 xzAbs, float lag, float span) {
    VegWind w;
    float t = camera.windParams.x;
    vec2 d = camera.windField.xy;
    w.dir = vec3(d.x, 0.0, d.y);
    w.side = vec3(-d.y, 0.0, d.x);
    w.trunk = vegDragAt(xzAbs, t - lag, true);
    w.kick = vegSmoothAbs(w.trunk - vegDragAt(xzAbs, t - lag - span, true));
    w.t = t;
    return w;
}

// A plant's wind: the drag at its root averaged over a tenth to four tenths
// of its trunk's period back (a quarter period's lag on the whole), so a
// heavy trunk does not follow what changes faster than it can (the two
// reads' mean is a low pass), and their difference, the change that sets
// it ringing.
VegWind vegWind(VegPlant plant) {
    float period = 1.0 / max(plant.trunk.freq, 0.05);
    vec2 xz = plant.root.xz + camera.eyeWorld.xz;
    VegWind w;
    float t = camera.windParams.x;
    vec2 d = camera.windField.xy;
    w.dir = vec3(d.x, 0.0, d.y);
    w.side = vec3(-d.y, 0.0, d.x);
    float a = vegDragAt(xz, t - min(0.1 * period, 0.5), true);
    float b = vegDragAt(xz, t - min(0.4 * period, 2.1), true);
    w.trunk = 0.5 * (a + b);
    w.kick = vegSmoothAbs(a - b);
    w.t = t;
    return w;
}

// A herb stalk's tip deflection (world metres) as a forced, damped
// oscillator, under drag `mean` (already lagged a quarter of its period)
// and the gusts' change `kick`. `turbulence` is the share of unresolved
// turbulence it feels (VEG_TURBULENCE),
// `phase` in cycles; `bob` how much it rises and falls as it rings (limbs
// and twigs; a trunk only leans).
//
// The ring: what sets it ringing, each as an RMS: the turbulence the gust
// field does not resolve, at its resonance (narrow-band response to a -5/3
// spectrum: mean drag times f^(-1/3) / sqrt(damping)); and the gusts' own
// change over the last half period, which only a slow level rings at. A
// lightly damped oscillator under broadband forcing answers as a narrow band
// of width about 2 zeta f round its frequency, so the ring is two sinusoids
// at f (1 -+ zeta): it swells and dies away over about 1 / (2 zeta f)
// rather than ringing on as a pure tone. Every frequency is fixed (the
// drag changes the ring's size, never its rate: scene_camera.glsl's
// oscillator clock).
vec3 vegLevelDrive(VegCantilever c, float zeta, float turbulence, float phase, float mean, float kick, VegWind w,
                   float bob) {
    if (c.sway <= 0.0 || c.freq <= 0.0) return vec3(0.0);
    float period = 1.0 / c.freq;
    float turb = turbulence * mean * pow(max(c.freq, 0.1), -1.0 / 3.0) * inversesqrt(zeta);
    float gust = VEG_GUST_KICK * kick * min(1.0, 1.25 * period);
    float rms = sqrt(turb * turb + gust * gust);
    float a1 = VEG_TWO_PI * (sceneWindCycles(c.freq * (1.0 - zeta)) + phase);
    float a2 = VEG_TWO_PI * (sceneWindCycles(c.freq * (1.0 + zeta)) + phase * 2.3) + 2.0;
    float a3 = VEG_TWO_PI * (sceneWindCycles(c.freq * 1.13) + phase * 3.7) + 1.3;
    float a4 = VEG_TWO_PI * (sceneWindCycles(c.freq * 0.97) + phase) + 2.1;
    vec3 x = w.dir * (mean + rms * (sin(a1) + sin(a2))) + w.side * (0.85 * rms * sin(a3)) +
             vec3(0.0, 1.41421 * bob * rms * sin(a4), 0.0);
    return x * (c.sway * c.len);
}

// The same driven by the plant's trunk drag (a herb's stalk).
vec3 vegLevel(VegCantilever c, float zeta, float phase, VegWind w, float bob) {
    return vegLevelDrive(c, zeta, VEG_TURBULENCE, phase, w.trunk, w.kick, w, bob);
}

// --- Trees and shrubs: a held lean, damped by size ---------------------------
//
// A tree's levels (vegPose) answer the wind as wood in leaf does. The drag
// bends each into a lean downwind that it holds while the wind blows and
// that changes as slowly as the wind's mean (the quasi-static bend, `mean`
// a quarter period back). Leaves damp what moves them hard, the more the
// smaller the part: a level's damping ratio is its wood's (VEG_*_DAMPING)
// plus the air's on its leaves, which grows with the wind and with how far
// the drag bends it for its stiffness (vegDamping). So a trunk rings weakly
// (0.15-0.3), a bough is near critically damped and a spray over-damped:
// what is quicker than the wind's changes tracks the drag, pushed and held,
// and the eddies too small for the gust field buffet it about its lean
// (vegBuffet: three slow sinusoids a part, fixed rates, irregular together)
// rather than set it ringing. Only a lightly damped level rings, as much as
// its damping lets (vegOver: none past critical).
const float VEG_AERO_DAMPING = 4.0;   // the air's damping on leaves over a streamlined crown's drag (they flap)
const float VEG_TREE_TURBULENCE = 0.04;
const float VEG_CROWN_RING = 0.07;
const float VEG_BUFFET = 0.2;        // the unresolved eddies' drag about its mean (rms), along the wind
const float VEG_EDDY_HZ = 0.7;       // a level slower than this averages them out
const float VEG_BOUGH_DAMPING = 0.2;
const float VEG_SPRAY_DAMPING = 0.35;

// The wind speed whose drag is `drag`.
float vegSpeed(float drag) { return VEG_REFERENCE_WIND * pow(max(drag, 0.0), 1.0 / 1.3); }

// A level's damping ratio under drag `mean`: its wood's `zeta0` and the
// air's on its leaves, rho Cd A U / (2 m w), which with the level's own
// terms (sway L = rho Cd A Uref^2 / (2 m w^2)) is VEG_AERO_DAMPING w (sway L)
// U / Uref^2.
float vegDamping(VegCantilever c, float zeta0, float mean) {
    return min(zeta0 + VEG_AERO_DAMPING * VEG_TWO_PI * c.freq * c.sway * c.len * vegSpeed(mean) /
                           (VEG_REFERENCE_WIND * VEG_REFERENCE_WIND), 2.0);
}
// The trunk's: its limbs take its energy, and the air on its crown more as
// the wind rises (0.15 in a breeze, 0.24 at 10 m/s).
float vegTrunkDamping(float mean) { return 0.12 + 0.012 * vegSpeed(mean); }

// How much a level rings at its own frequency for its damping: none once
// it is critically damped.
float vegOver(float zeta) { return 1.0 - smoothstep(0.3, 0.8, zeta); }

// The unresolved eddies' buffeting, along and across the wind, each about
// 1 rms: slow sinusoids at fixed rates (scene_camera.glsl's clock), a limb's
// own spread of them (from `seed`, one a limb: points of one spray at rates
// even slightly apart drift apart over the clock and part the leaves from
// their twigs) and the point's own phases.
vec2 vegBuffet(float phase, float seed) {
    float s = 0.85 + 0.3 * fract(seed * 7.31);
    float b1 = sin(VEG_TWO_PI * (sceneWindCycles(0.37 * s) + phase));
    float b2 = sin(VEG_TWO_PI * (sceneWindCycles(0.61 * s) + phase * 2.7) + 1.1);
    float b3 = sin(VEG_TWO_PI * (sceneWindCycles(1.01 * s) + phase * 4.1) + 2.3);
    float c1 = sin(VEG_TWO_PI * (sceneWindCycles(0.43 * s) + phase * 1.9) + 0.7);
    float c2 = sin(VEG_TWO_PI * (sceneWindCycles(0.79 * s) + phase * 3.3) + 2.9);
    return vec2((b1 + 0.7 * b2 + 0.45 * b3) * 1.088, (c1 + 0.6 * c2) * 1.213);
}

// A tree level's tip deflection (world metres): its held lean under `mean`,
// buffeted (`buffet`: 0 for a trunk, which averages the eddies over its
// crown), ringing as its damping lets: the unresolved turbulence at its
// resonance (narrow band: mean f^-1/3 / sqrt(zeta)) and the gusts' change
// `kick`. `seed`: the limb's (vegBuffet).
vec3 vegTreeLevel(VegCantilever c, float zeta, float ring, float phase, float mean, float kick, VegWind w, float bob,
                  float buffet, float seed) {
    if (c.sway <= 0.0 || c.freq <= 0.0) return vec3(0.0);
    float over = vegOver(zeta);
    float rms = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, a4 = 0.0;
    if (over > 0.0) {
        float turb = ring * mean * pow(max(c.freq, 0.1), -1.0 / 3.0) * inversesqrt(zeta);
        float gust = VEG_GUST_KICK * kick * min(1.0, 1.25 / c.freq) * sqrt(0.15 / zeta);
        rms = over * sqrt(turb * turb + gust * gust);
        // A fixed band about its frequency: the damping follows the wind,
        // and a frequency that followed it would chirp (its phase is its
        // frequency times a clock hundreds of seconds long).
        a1 = sin(VEG_TWO_PI * (sceneWindCycles(c.freq * 0.94) + phase));
        a2 = sin(VEG_TWO_PI * (sceneWindCycles(c.freq * 1.06) + phase * 2.3) + 2.0);
        a3 = sin(VEG_TWO_PI * (sceneWindCycles(c.freq * 1.13) + phase * 3.7) + 1.3);
        a4 = sin(VEG_TWO_PI * (sceneWindCycles(c.freq * 0.97) + phase) + 2.1);
    }
    vec2 b = vec2(0.0);
    if (buffet > 0.0) {
        float lp = c.freq * c.freq / (c.freq * c.freq + VEG_EDDY_HZ * VEG_EDDY_HZ);
        b = vegBuffet(phase, seed) * vec2(VEG_BUFFET, 0.7 * VEG_BUFFET) * (buffet * mean * lp);
    }
    vec3 x = w.dir * (mean + b.x + rms * (a1 + a2)) + w.side * (b.y + 0.85 * rms * a3) +
             vec3(0.0, bob * (1.41421 * rms * a4 + 0.5 * b.y), 0.0);
    return x * (c.sway * c.len);
}

// The movers' push at camera-relative rest point `p`, `above` metres over
// its plant's root (scene/plant_movers.h). Each sample of a mover's trail
// pushes the point away from the body's axis as far as the body is deep in
// it, through a damped spring's impulse response at the sample's age: the
// plant parts as the body arrives, holds while it stays and swings back
// past rest once after it goes. Horizontal, and a little down (a pushed
// shoot bends down as it goes aside). A point feels the body from
// VEG_MOVER_REACH of its radius (scene_view.cpp sizes each mover's bound
// with it); a quicker, stiffer spring parted a shrub by a hand's breadth a
// frame.
const float VEG_MOVER_HZ = 1.0;
const float VEG_MOVER_DAMPING = 0.4;
const float VEG_MOVER_REACH = 1.6;
const float VEG_MOVER_STEP = 0.1;   // PlantMovers::kStep
// The damped spring's step response at time t (0 before it).
float vegMoverStep(float t, float w, float z, float wd) {
    if (t <= 0.0) return 0.0;
    return 1.0 - exp(-z * w * t) * (cos(wd * t) + z * w / wd * sin(wd * t));
}
vec3 vegMoverPush(vec3 p, float above) {
    int n = int(camera.plantMoverInfo.x + 0.5);
    if (n <= 0) return vec3(0.0);
    int K = int(camera.plantMoverInfo.y + 0.5);
    float w = VEG_TWO_PI * VEG_MOVER_HZ, z = VEG_MOVER_DAMPING;
    float wd = w * sqrt(1.0 - z * z);
    vec2 acc = vec2(0.0);
    for (int m = 0; m < n; ++m) {
        vec4 a = camera.plantMovers[m * 2], b = camera.plantMovers[m * 2 + 1];
        vec2 dm = p.xz - a.xy;
        if (dot(dm, dm) > a.z * a.z || above > b.x) continue;
        float R = a.w, reach = VEG_MOVER_REACH * R;
        vec2 sum = vec2(0.0);
        for (int k = 0; k < K; ++k) {
            vec4 s = camera.plantTrail[m * K + k];
            if (s.w <= 0.0) continue;
            vec2 d = p.xz - s.xy;
            float r = length(d);
            if (r >= reach) continue;
            vec2 dir = r > 1e-4 ? d / r : vec2(1.0, 0.0);
            // The spring's response to the push over the sample's whole
            // step (its step response at the age less a step earlier), not
            // an impulse at its time: each new sample then starts the plant
            // moving with no kick in its speed (an impulse's response rises
            // at once, and a shrub jerked aside every 0.1 s as it parted).
            // The trail's oldest half second fades so a sample leaves softly.
            float h = vegMoverStep(s.z, w, z, wd) - vegMoverStep(s.z - VEG_MOVER_STEP, w, z, wd);
            h *= 1.0 - smoothstep(1.5, 2.0, s.z);
            sum += dir * (R * (1.0 - smoothstep(0.0, reach, r)) * h * s.w);
        }
        acc += sum * b.y * (1.0 - smoothstep(b.x - 0.3, b.x, above));
    }
    return vec3(acc.x, -0.25 * length(acc), acc.y);
}

// The bent shape of a cantilever s of the way along it, and its slope.
float vegBend(float s) { return s <= 1.0 ? s * s * (3.0 - s) * 0.5 : 1.0 + 1.5 * (s - 1.0); }
float vegBendSlope(float s) { return s <= 1.0 ? s * (3.0 - 1.5 * s) : 1.5; }

// The trunk's bend at `h` of its height: h^p (broflora's swayStem, from the
// stem's own taper: a slim top bends most), or the uniform beam's when
// unset (p 0); on past the top along its slope.
float vegTrunkBend(float h, float p) {
    if (p <= 0.0) return vegBend(h);
    return h <= 1.0 ? pow(max(h, 0.0), p) : 1.0 + p * (h - 1.0);
}
float vegTrunkBendSlope(float h, float p) {
    if (p <= 0.0) return vegBendSlope(h);
    return h <= 1.0 ? p * pow(max(h, 1e-4), p - 1.0) : p;
}

// The wood's pose at camera-relative rest point `rest`, `weight` along its
// limb (0 on the trunk). The shared layer: see the header.
VegPose vegPose(VegPlant plant, VegLimb limb, VegWind w, vec3 rest, float weight) {
    VegPose o;
    o.drag = w.trunk;
    // The trunk, bending from the root in its stem's shape; its top drops a
    // little as it leans.
    float h = clamp((rest.y - plant.root.y) / plant.height, 0.0, 1.3);
    vec3 xt = vegTreeLevel(plant.trunk, vegTrunkDamping(w.trunk), VEG_TREE_TURBULENCE, plant.seed, w.trunk, w.kick, w,
                           0.0, 0.0, plant.seed);
    float bt = vegTrunkBend(h, plant.shape);
    o.offset = xt * bt - vec3(0.0, 0.6 * dot(xt, xt) * bt / plant.height, 0.0);
    o.omega = cross(vec3(0.0, 1.0, 0.0), xt) * (vegTrunkBendSlope(h, plant.shape) / plant.height);
    if (limb.limb.sway <= 0.0 || weight <= 0.0) return o;

    // The limb, bending from where it leaves the trunk. Its direction here
    // is taken as outward from the trunk and up. It feels the wind where it
    // is, not at the root: a gust reaches the upwind side of a crown first
    // and travels through it (about a second across a broad crown), so the
    // limbs follow one another rather than moving with the trunk as one.
    vec3 rel = rest - plant.root;
    float out2 = dot(rel.xz, rel.xz);
    vec3 dir = out2 > 1e-6 ? normalize(vec3(rel.x, 0.75 * sqrt(out2), rel.z)) : vec3(0.0, 1.0, 0.0);
    float s = clamp(weight, 0.0, 1.0);
    vec2 here = rest.xz + camera.eyeWorld.xz;
    float pl = vegDragAt(here, w.t - vegLag(limb.limb.freq), true);
    o.drag = pl;
    vec3 xl = vegTreeLevel(limb.limb, vegDamping(limb.limb, VEG_BOUGH_DAMPING, pl), VEG_CROWN_RING, limb.phase, pl,
                           w.kick, w, vegLimbBob, 1.0, limb.phase);
    o.offset += xl * vegBend(s);
    // A body pushing through it moves its outer shoots and leaves aside
    // (the wood near the trunk stays).
    o.offset += vegMoverPush(rest, rest.y - plant.root.y) * smoothstep(0.15, 0.7, s);
    o.omega += cross(dir, xl) * (vegBendSlope(s) / max(limb.limb.len, 0.05));

    // Its twigs: the outer end of the limb, each on a phase of its own that
    // drifts smoothly through the crown (so a twig and its leaves move
    // together and neighbouring twigs do not), in the limb's wind.
    if (limb.twig.sway <= 0.0) return o;
    float w0 = 1.0 - min(1.0, 1.5 * limb.twig.len / max(limb.limb.len, 0.05));
    float u = clamp((s - w0) / max(1.0 - w0, 1e-3), 0.0, 1.0);
    if (u <= 0.0) return o;
    vec3 ra = rest + camera.eyeWorld.xyz;
    float ph = limb.phase + 0.12 * (sin(dot(ra, vec3(3.1, 1.7, 2.3))) + sin(dot(ra, vec3(-1.9, 2.9, 3.7))) +
                                    sin(dot(ra, vec3(2.7, -3.3, 1.3))));
    vec3 xw = vegTreeLevel(limb.twig, vegDamping(limb.twig, VEG_SPRAY_DAMPING, pl), VEG_CROWN_RING, ph, pl, w.kick, w,
                           vegTwigBob, 1.0, limb.phase);
    o.offset += xw * vegBend(u);
    o.omega += cross(dir, xw) * (vegBendSlope(u) / max(limb.twig.len, 0.02));
    return o;
}

// Rotation by `ang` about unit `axis` (Rodrigues), as a matrix.
mat3 vegAxisAngle(vec3 axis, float ang) {
    float c = cos(ang), s = sin(ang), t = 1.0 - c;
    vec3 a = axis;
    return mat3(t * a.x * a.x + c,       t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y,
                t * a.x * a.y - s * a.z, t * a.y * a.y + c,       t * a.y * a.z + s * a.x,
                t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c);
}

// The rotation of a rotation vector.
mat3 vegRotation(vec3 omega) {
    float a = length(omega);
    return a > 1e-6 ? vegAxisAngle(omega / a, a) : mat3(1.0);
}

// The near tier's leaf on top of its wood's pose: `axis` its stem (tip)
// direction and `normal` its card normal (world, unit), `local` the vertex's
// offset from where it hangs, `seed` in [0, 1) its own. Returns the rotation
// the vertex's offset and its normal and tangent take:
//   - the wood's rotation where it hangs (pose.omega), so it turns with its
//     limb and its shading follows the bend;
//   - flutter about the petiole: a twist about its stem and a smaller pitch,
//     at its own fixed 2-4.5 Hz on its own phase, larger in stronger wind
//     (the drag at its limb, pose.drag), in bursts;
//   - in a gust (drag past a light breeze's), turned part of the way
//     downwind and folded along its midrib, both halves rising.
//
// vegLeafAmount scales the leaf's own motion (the downwind turn, flutter and
// fold) by `amount`, so a tier that hands leaves over to coarser ones fades
// it out rather than dropping it; vegLeaf is amount 1.
// The leaf's turn as a whole (the wood's rotation, the downwind turn and the
// flutter), and its fold: each half turns a further `fold` radians about the
// stem, the half on `side`'s positive side by +fold, the other by -fold
// (vegLeafAmount's last step). A forest's leaves are posed by this once a
// leaf (forest_leaf_pose.comp), not once a vertex.
mat3 vegLeafTurn(VegPose pose, VegWind w, vec3 axis, vec3 normal, float seed, float amount, out vec3 side,
                 out float fold) {
    fold = 0.0;
    side = vec3(1.0, 0.0, 0.0);
    if (amount <= 0.0) return vegRotation(pose.omega);
    float drag = pose.drag;
    float gust = smoothstep(0.0, 1.0, (drag - 0.12) * 1.6) * amount;
    vec3 omega = pose.omega;
    vec3 turn = cross(axis, w.dir);
    float tl = length(turn);
    if (tl > 1e-4) omega += turn * (min(atan(tl, dot(axis, w.dir)) * 0.45, 1.0) * gust / tl);

    side = cross(normal, axis);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : vec3(1.0, 0.0, 0.0);
    // Its own frequency, fixed (a leaf on its petiole flutters at 2-4.5 Hz;
    // the wind sets how far, in bursts, never how fast: a frequency that
    // followed the wind would chirp, scene_camera.glsl's oscillator clock).
    float f = VEG_LEAF_FLUTTER_HZ.x + VEG_LEAF_FLUTTER_HZ.y * fract(seed * 7.13);
    float a = amount * 0.35 * sqrt(max(drag, 0.0)) *
              (0.55 + 0.45 * sin(VEG_TWO_PI * (sceneWindCycles(0.45 + 0.3 * fract(seed * 3.7)) + seed)));
    float th = VEG_TWO_PI * (sceneWindCycles(f) + seed);
    // (Each sinusoid its own cycles: a multiple of a wrapped phase would jump.)
    float th2 = VEG_TWO_PI * (sceneWindCycles(1.37 * f) + 1.37 * seed) + 2.0;
    omega += axis * (a * sin(th)) + side * (0.45 * a * sin(th2));
    fold = 0.8 * gust;
    return vegRotation(omega);
}

mat3 vegLeafAmount(VegPose pose, VegWind w, vec3 axis, vec3 normal, vec3 local, float seed, float amount) {
    vec3 side;
    float fold;
    mat3 R = vegLeafTurn(pose, w, axis, normal, seed, amount, side, fold);
    float x = dot(local, side);
    if (fold > 0.0 && abs(x) > 1e-6) R = R * vegAxisAngle(axis, sign(x) * fold);
    return R;
}

mat3 vegLeaf(VegPose pose, VegWind w, vec3 axis, vec3 normal, vec3 local, float seed) {
    return vegLeafAmount(pose, w, axis, normal, local, seed, 1.0);
}

// The crown as one form, for a leaf hanging at camera-relative `anchor`:
// the shading varying's bulk normal (toward the crown's centre-out
// direction) and its ambient occlusion (leaves deep inside take less sky),
// negative as a leaf's. v1 is the record's crown; radius 0: no bulk normal.
vec4 vegCrownShade(mat4 model, vec4 v1, vec3 anchor) {
    if (v1.w <= 0.0) return vec4(0.0, 0.0, 0.0, -1.0);
    float scale = vegScale(model);
    vec3 crown = (model * vec4(v1.xyz, 1.0)).xyz;
    vec3 rc = (anchor - crown) / max(v1.w * scale, 0.5);
    vec3 bulk = normalize(rc + vec3(0.0, 0.25, 0.0));
    float depth = clamp(length(rc), 0.0, 1.2);
    float ao = clamp(0.25 + 0.85 * depth * depth, 0.25, 1.0) * mix(0.75, 1.0, clamp(rc.y * 0.5 + 0.6, 0.0, 1.0));
    return vec4(bulk * 0.6, -ao);
}

// Sway one vertex: the wood's pose, and on a leaf the leaf's own layer.
// `pos` is its camera-relative rest position, bent in place; `axis` the
// leaf's stem direction in node space (zero on wood) and `leafNormal` its
// card normal (world); `rot` receives the rotation its normal and tangent
// take. Returns the shading varying mesh.frag reads under SCENE_VEGETATION:
// xyz the bulk normal times its weight (zero on wood or without a crown),
// w the ambient occlusion, negative on a leaf.
vec4 vegetationSwayAxis(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, vec3 axis, vec3 leafNormal,
                        inout vec3 pos, out mat3 rot) {
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    bool leaf = dot(axis, axis) > 0.25;

    float bw = clamp(v2.w, 0.0, 1.0);
    if (!leaf && v1.w < 0.0) {
        // A wood segment record (one record over a whole segment, an
        // instanced cylinder): v2 holds its base and the weight there, v1 its
        // tip and -(1 + the weight there); each vertex takes the weight at
        // its own point along it, so the tip bends as its child's base does.
        vec3 tip = (model * vec4(v1.xyz, 1.0)).xyz;
        vec3 d = tip - anchor;
        float dd = dot(d, d);
        float along = dd > 1e-12 ? clamp(dot(pos - anchor, d) / dd, 0.0, 1.0) : 1.0;
        bw = mix(bw, clamp(-v1.w - 1.0, 0.0, 1.0), along);
    }

    if (!leaf) {
        VegPose pose = vegPose(plant, limb, w, pos, bw);
        float h = clamp((pos.y - plant.root.y) / plant.height, 0.0, 1.0);
        pos += pose.offset;
        rot = vegRotation(pose.omega);
        // Bark darkens toward the ground.
        return vec4(0.0, 0.0, 0.0, mix(0.6, 1.0, h));
    }

    VegPose pose = vegPose(plant, limb, w, anchor, bw);
    float seed = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    rot = vegLeaf(pose, w, normalize(mat3(model) * axis), leafNormal, pos - anchor, seed);
#ifdef VEG_SHADOW_CASTER
    pos = anchor + pose.offset + rot * (pos - anchor);
#else
    vec3 at = anchor + pose.offset;
    pos = at + rot * (pos - anchor) * smoothstep(VEG_NEAR_SHRINK.x, VEG_NEAR_SHRINK.y, length(at));
#endif

    // The crown as one form: the leaf's normal leans toward the crown's
    // centre-out direction, and leaves deep inside take less sky.
    return vegCrownShade(model, v1, anchor);
}

// The same with the stem axis from the record's packed word (v3.x).
vec4 vegetationSway(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, vec3 leafNormal, inout vec3 pos, out mat3 rot) {
    vec3 axis = v3.x > 0.5 ? vegUnpackAxis(v3.x) : vec3(0.0);
    return vegetationSwayAxis(model, v0, v1, v2, v3, axis, leafNormal, pos, rot);
}

// The colour a plant's part takes on top of its material, from the same
// records: rgb multiplies the base colour (the vertex shaders fold it into
// the instance tint). A tree is never one flat hue:
//   - each plant leans warm (yellow-green) or cool (blue-green), hashed from
//     its root;
//   - each limb is lighter or darker (its phase), and each leaf a little;
//   - leaves deep in the crown are darker and cooler, the rim lighter, so a
//     crown is dark inside and lit at its edge.
// Bark varies by plant, and darkens toward the ground.
//
// The per-tree part (vegTreeTone: a hash of where the plant stands, and the
// leaf and bark tints from it) is split out so a forest's clump lobes, which
// place their own leaves (scene_forest_mass.glsl), take the same.
float vegTreeTone(mat4 model, vec4 v0) {
    vec3 rootAbs = (model * vec4(v0.xyz, 1.0)).xyz + camera.eyeWorld.xyz;
    return sceneWindHash(ivec2(floor(rootAbs.xz * 8.0)) + ivec2(17, 3));
}
vec3 vegTreeLeafTint(float tree) {
    // Warm or cool, and a little lighter or darker.
    float value = 0.9 + 0.2 * fract(tree * 13.7);
    return mix(vec3(0.84, 0.97, 1.08), vec3(1.12, 1.04, 0.76), tree) * value;
}
vec3 vegTreeBarkTint(float tree) { return vec3(0.82 + 0.3 * tree, 0.8 + 0.28 * tree, 0.78 + 0.24 * tree); }

// A leaf's tint before its own and its depth in the crown: the tree's, and
// its limb's (a forest lobe adds the rest leaf by leaf, scene_forest_mass.glsl).
vec3 vegLimbLeafTint(mat4 model, vec4 v0, vec4 v3) {
    float phase = float(uint(max(v3.y, 0.0) + 0.5) >> 18) / 64.0;
    float limb = fract(phase * 7.31 + 0.37);
    return vegTreeLeafTint(vegTreeTone(model, v0)) * (0.8 + 0.4 * limb);
}

vec3 vegetationTint(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, bool leaf) {
    float scale = vegScale(model);
    if (!leaf) return vegTreeBarkTint(vegTreeTone(model, v0));

    float each = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    vec3 c = vegLimbLeafTint(model, v0, v3);
    c *= 0.9 + 0.2 * each;
    if (v1.w > 0.0) {
        vec3 crown = (model * vec4(v1.xyz, 1.0)).xyz;
        vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
        float depth = length(anchor - crown) / max(v1.w * scale, 0.5);
        c *= mix(vec3(0.5, 0.62, 0.68), vec3(1.0), smoothstep(0.25, 1.0, depth));
        c *= 1.0 + 0.15 * smoothstep(0.85, 1.15, depth);
    }
    return c;
}
