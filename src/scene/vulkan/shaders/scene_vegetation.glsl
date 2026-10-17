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
// leafy sprays). broflora gives each its natural frequency (its tip
// stiffness 3 E I / L^3 over the mass its mode moves: Euler-Bernoulli's
// f ~ d / L^2 for a bare beam, slower for what it carries; the trunk is
// long and holds the crown, so slow; sprays are short, so quick) and its
// static tip deflection over its length at 10 m/s, from the drag on the
// leaves it carries over that stiffness (so a thick trunk deflects
// little). Here each level is driven by the drag at the plant, (U / 10
// m/s)^1.3 (a crown streamlines, so drag grows slower than U^2), and
// answers as a forced, damped oscillator: its static deflection under the
// drag a quarter period back (a heavy level lags the gusts that reach it),
// plus ringing at its own frequency, from the turbulence too fine for the
// gust field (strongest on the slow, lightly damped levels) and from how
// much the drag changed in its last half period, damped by its ratio (the
// trunk most: its limbs soak up its energy, "multiple mass damping"). The
// drag's past is the travelling gust field read at earlier
// times: three reads a vertex, shared by the levels. A level's bend has the
// shape of a loaded cantilever, s^2 (3 - s) / 2 along it, so the bend is at
// its outer end, and the levels add down the hierarchy.
//
// Two layers, so a level-of-detail chain can share one pose:
//   vegPose   the trunk, limb and twig bend at a rest point: the offset and
//             the rotation of the wood there. Every tier (leaves, clump
//             cards, impostors) calls it with the same per-plant data
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
const float VEG_TWO_PI = 6.28318530718;

// Damping ratios. The trunk's is high because its limbs and leaves take
// its energy (James et al.); a limb has twigs to give it to; a twig has
// only its leaves.
const float VEG_TRUNK_DAMPING = 0.15;
const float VEG_LIMB_DAMPING = 0.08;
const float VEG_TWIG_DAMPING = 0.05;

// The ringing (vegLevel). VEG_TURBULENCE: unresolved turbulence of 15% of
// the drag above 0.1 Hz with a -5/3 spectrum, whose narrow-band resonant
// RMS is sigma sqrt(pi / (4 * 6.76)) times the mean drag at 1 Hz and
// damping 1. VEG_GUST_KICK: the share of a gust's change that rings. Both
// were checked against a damped oscillator integrated under the same drag
// (the grove's sway_check.js: deflection RMS and speed within ~15%).
const float VEG_TURBULENCE = 0.051;
const float VEG_GUST_KICK = 0.4;

// A cantilever: natural frequency (Hz), tip deflection over length at
// VEG_REFERENCE_WIND (0: it does not bend), length (world metres).
struct VegCantilever { float freq; float sway; float len; };

// What every vertex of a plant shares: its root (camera-relative) and
// height (world), its trunk, and a hash of where it stands.
struct VegPlant { vec3 root; float height; VegCantilever trunk; float seed; };

// What every vertex of a limb shares: the limb and its twigs, its phase.
struct VegLimb { VegCantilever limb; VegCantilever twig; float phase; };

// The wind at a plant: downwind and across (horizontal, unit), and the drag
// (U / 10 m/s)^1.3 now, 0.4 s ago and 1.3 s ago (the past from the slow part
// of the travelling gusts), and the clock.
struct VegWind { vec3 dir; vec3 side; float p0; float p1; float p2; float t; };

// The wood's pose at a point: the offset it moves by and its rotation (a
// rotation vector: axis times radians).
struct VegPose { vec3 offset; vec3 omega; };

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
    p.trunk = VegCantilever(vegLogValue(w >> 12, 12u, VEG_FREQ_RANGE) / scale,
                            vegLogValue(w & 4095u, 12u, VEG_SWAY_RANGE), p.height);
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

VegWind vegWind(VegPlant plant) {
    VegWind w;
    vec2 xz = plant.root.xz + camera.eyeWorld.xz;
    float t = camera.windParams.x;
    vec2 d = camera.windField.xy;
    float u = camera.windField.z;
    w.dir = vec3(d.x, 0.0, d.y);
    w.side = vec3(-d.y, 0.0, d.x);
    w.p0 = vegPressure(u + 4.0 * sceneWindGustAtTime(xz, t, true));
    w.p1 = vegPressure(u + 4.0 * sceneWindGustAtTime(xz, t - 0.4, false));
    w.p2 = vegPressure(u + 4.0 * sceneWindGustAtTime(xz, t - 1.3, false));
    w.t = t;
    return w;
}

// One level's tip deflection (world metres) as a forced, damped oscillator
// (see the model above). `phase` in cycles; `bob` how much it rises and
// falls as it rings (limbs and twigs; a trunk only leans).
vec3 vegLevel(VegCantilever c, float zeta, float phase, VegWind w, float bob) {
    if (c.sway <= 0.0 || c.freq <= 0.0) return vec3(0.0);
    float period = 1.0 / c.freq;
    // The drag a quarter period back, from the three reads.
    float lag = min(0.25 * period, 1.3);
    float mean = lag < 0.4 ? mix(w.p0, w.p1, lag * 2.5) : mix(w.p1, w.p2, (lag - 0.4) / 0.9);
    // What sets it ringing, each as an RMS: the turbulence the gust field
    // does not resolve, at its resonance (narrow-band response to a -5/3
    // spectrum: mean drag times f^(-1/3) / sqrt(damping)); and the gusts'
    // own change over its last half period, which only a level slower than
    // the change rings at. The ring is a sinusoid of their combined RMS.
    float slow = clamp((0.5 * period - 0.4) / 0.9, 0.0, 1.0);
    float kick = mix(abs(w.p0 - w.p1), abs(w.p0 - w.p1) + abs(w.p1 - w.p2), slow);
    float turb = VEG_TURBULENCE * mean * pow(max(c.freq, 0.1), -1.0 / 3.0) * inversesqrt(zeta);
    float gust = VEG_GUST_KICK * kick * min(1.0, 1.25 * period);
    float ring = 1.41421 * sqrt(turb * turb + gust * gust);
    float th = VEG_TWO_PI * (c.freq * w.t + phase);
    float th2 = VEG_TWO_PI * (c.freq * 1.13 * w.t + phase * 3.7) + 1.3;
    vec3 x = w.dir * (mean + ring * sin(th)) + w.side * (0.6 * ring * sin(th2)) +
             vec3(0.0, bob * ring * sin(th * 0.97 + 2.1), 0.0);
    return x * (c.sway * c.len);
}

// The bent shape of a cantilever s of the way along it, and its slope.
float vegBend(float s) { return s <= 1.0 ? s * s * (3.0 - s) * 0.5 : 1.0 + 1.5 * (s - 1.0); }
float vegBendSlope(float s) { return s <= 1.0 ? s * (3.0 - 1.5 * s) : 1.5; }

// The wood's pose at camera-relative rest point `rest`, `weight` along its
// limb (0 on the trunk). The shared layer: see the header.
VegPose vegPose(VegPlant plant, VegLimb limb, VegWind w, vec3 rest, float weight) {
    VegPose o;
    // The trunk, bending from the root; its top drops a little as it leans.
    float h = clamp((rest.y - plant.root.y) / plant.height, 0.0, 1.3);
    vec3 xt = vegLevel(plant.trunk, VEG_TRUNK_DAMPING, plant.seed, w, 0.0);
    float bt = vegBend(h);
    o.offset = xt * bt - vec3(0.0, 0.6 * dot(xt, xt) * bt / plant.height, 0.0);
    o.omega = cross(vec3(0.0, 1.0, 0.0), xt) * (vegBendSlope(h) / plant.height);
    if (limb.limb.sway <= 0.0 || weight <= 0.0) return o;

    // The limb, bending from where it leaves the trunk. Its direction here
    // is taken as outward from the trunk and up.
    vec3 rel = rest - plant.root;
    float out2 = dot(rel.xz, rel.xz);
    vec3 dir = out2 > 1e-6 ? normalize(vec3(rel.x, 0.75 * sqrt(out2), rel.z)) : vec3(0.0, 1.0, 0.0);
    float s = clamp(weight, 0.0, 1.0);
    vec3 xl = vegLevel(limb.limb, VEG_LIMB_DAMPING, limb.phase, w, 0.6);
    o.offset += xl * vegBend(s);
    o.omega += cross(dir, xl) * (vegBendSlope(s) / max(limb.limb.len, 0.05));

    // Its twigs: the outer end of the limb, each on a phase of its own that
    // drifts smoothly through the crown (so a twig and its leaves move
    // together and neighbouring twigs do not).
    if (limb.twig.sway <= 0.0) return o;
    float w0 = 1.0 - min(1.0, 1.5 * limb.twig.len / max(limb.limb.len, 0.05));
    float u = clamp((s - w0) / max(1.0 - w0, 1e-3), 0.0, 1.0);
    if (u <= 0.0) return o;
    vec3 ra = rest + camera.eyeWorld.xyz;
    float ph = limb.phase + 0.12 * (sin(dot(ra, vec3(3.1, 1.7, 2.3))) + sin(dot(ra, vec3(-1.9, 2.9, 3.7))) +
                                    sin(dot(ra, vec3(2.7, -3.3, 1.3))));
    vec3 xw = vegLevel(limb.twig, VEG_TWIG_DAMPING, ph, w, 0.8);
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
//     at 3-9 Hz on its own phase, larger in stronger wind, in bursts;
//   - in a gust (drag past a light breeze's), turned part of the way
//     downwind and folded along its midrib, both halves rising.
//
// vegLeafAmount scales the leaf's own motion (the downwind turn, flutter and
// fold) by `amount`, so a tier that hands leaves over to coarser ones fades
// it out rather than dropping it; vegLeaf is amount 1.
mat3 vegLeafAmount(VegPose pose, VegWind w, vec3 axis, vec3 normal, vec3 local, float seed, float amount) {
    if (amount <= 0.0) return vegRotation(pose.omega);
    float gust = clamp((w.p0 - 0.12) * 1.6, 0.0, 1.0) * amount;
    vec3 omega = pose.omega;
    vec3 turn = cross(axis, w.dir);
    float tl = length(turn);
    if (tl > 1e-4) omega += turn * (min(atan(tl, dot(axis, w.dir)) * 0.45, 1.0) * gust / tl);

    vec3 side = cross(normal, axis);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : vec3(1.0, 0.0, 0.0);
    float f = 3.0 + 4.0 * fract(seed * 7.13) + 2.5 * min(w.p0, 1.0);
    float a = amount * 0.35 * sqrt(w.p0) * (0.55 + 0.45 * sin(VEG_TWO_PI * (0.6 * w.t + seed)));
    float th = VEG_TWO_PI * (f * w.t + seed);
    omega += axis * (a * sin(th)) + side * (0.45 * a * sin(1.37 * th + 2.0));

    mat3 R = vegRotation(omega);
    float x = dot(local, side);
    if (gust > 0.0 && abs(x) > 1e-6) R = R * vegAxisAngle(axis, sign(x) * 0.8 * gust);
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
    pos = anchor + pose.offset + rot * (pos - anchor);

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
// leaf and bark tints from it) is split out so a forest impostor, whose
// picture holds the rest (forest_impostor.cpp's leafTint, which follows
// this line for line), takes the same: everything else about a leaf's
// colour belongs to its specimen.
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

vec3 vegetationTint(mat4 model, vec4 v0, vec4 v1, vec4 v2, vec4 v3, bool leaf) {
    float scale = vegScale(model);
    float tree = vegTreeTone(model, v0);
    float phase = float(uint(max(v3.y, 0.0) + 0.5) >> 18) / 64.0;
    float limb = fract(phase * 7.31 + 0.37);
    if (!leaf) return vegTreeBarkTint(tree);

    float each = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    vec3 c = vegTreeLeafTint(tree);
    c *= 0.8 + 0.4 * limb;
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
