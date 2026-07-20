// Clipmap terrain — procedural detail below the data floor.
//
// The height pyramid bottoms out at whatever the finest layer's cell size is
// (30 m for the worldgen source). Standing on the ground, 30 m is roughly a
// city block: the surface reads as a smooth ramp with no relief at any scale a
// person actually sees. This synthesises the missing decades deterministically
// on the GPU.
//
// Concatenated in front of BOTH the vertex and fragment chunks, like
// clipmap_common.glsl and for the same reason: the displaced surface and the
// shaded surface have to be the same surface.
//
// Two properties make it composable with the clipmap's crack-free guarantee:
//
//   1. It is a pure function of world XZ, so two rings meeting at a boundary
//      still agree.
//   2. Every octave is BAND-LIMITED against the same cell size that selects
//      the height mip. An octave whose wavelength approaches the rendered cell
//      fades to zero instead of aliasing, and because cell size is continuous
//      in distance, octaves fade in smoothly as you approach rather than pop.
//
// It also carries an analytic derivative, so the fragment stage gets the
// detail's contribution to the normal exactly, with no extra taps.

uniform vec2  u_detailOffset;      // camXZ - u_detailAnchor  (small; see below)
uniform vec2  u_detailAnchor;      // camXZ snapped to the anchor grid (ClipmapTerrain::kDetailAnchor)
uniform float u_detailWavelength;  // coarsest detail octave, metres
uniform float u_detailRelief;      // detail slope as a fraction of the ground's
uniform float u_detailGain;        // amplitude ratio between octaves
uniform float u_detailOctaves;

// The learned detail exemplar — see cmExemplar below. Declared up here because
// cmDetail has to know whether it exists: the patch supersedes the octaves that
// would otherwise climb toward the data floor.
uniform sampler2D u_exemplar;
uniform float     u_exN;         // texels per repeat
uniform float     u_exPresent;
uniform float     u_exLambda;    // repeat length of the coarse tap, metres

// Enough to span the whole pyramid: the band runs from just under the coarsest
// data cell (7.68 km for the worldgen source) down past the finest the eye can
// resolve. Only the octaves inside the band-pass window are evaluated — the
// window is a handful of octaves wide anywhere, so the loop bound is a ceiling,
// not a cost.
const int CM_DETAIL_MAX_OCTAVES = 20;

// Octaves COARSER than u_detailWavelength, i.e. how far the band can climb when
// the data underfoot is coarse. 2^8 * 48 m = 12 km, past the coarse cell.
const int CM_DETAIL_UP_OCTAVES = 8;

// Fade window, as a fraction of an octave's wavelength. An octave is at full
// strength while the rendered cell is under 0.25 of its wavelength and gone by
// 0.6 — comfortably inside Nyquist, with the ramp wide enough that the fade is
// not itself visible as a moving front.
// Ground is never a mathematical plane. Even a meadow has relief at metre
// scale, so the slope modulator has a floor: below it, detail stops being
// proportional to the landscape and becomes a property of the material.
const float CM_DETAIL_FLOOR = 0.12;

const float CM_DETAIL_FADE_LO = 0.25;
const float CM_DETAIL_FADE_HI = 0.60;

// ---------------------------------------------------------------------------
// Hashed gradients.
//
// Integer bit-mixing on the lattice cell, NOT the usual fract(sin(dot(p,k)))
// trick. This world is hundreds of kilometres across; at a sub-metre
// wavelength the noise coordinate reaches ~1e6, where fp32 sin() has no
// meaningful precision left and the field degenerates into stripes.
// ---------------------------------------------------------------------------

uint cmHashU(ivec2 c) {
    uvec2 v = uvec2(c + 0x2000000);      // bias: the mix wants unsigned
    uint h = v.x * 0x8da6b343u + v.y * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

vec2 cmGradient(ivec2 c) {
    float a = float(cmHashU(c) & 0xffffu) * (6.2831853 / 65536.0);
    return vec2(cos(a), sin(a));
}

// Gradient noise in [-1,1] with its exact derivative: vec3(value, d/dx, d/dz).
//
// `p` is the lattice coordinate and `cellOfs` an integer offset added to the
// lattice cell. Splitting them is what keeps precision: `p` stays small
// (near-camera, where fine octaves are alive at all) while the world's absolute
// position rides in an integer that costs nothing to be large.
vec3 cmNoiseD(vec2 p, ivec2 cellOfs) {
    vec2  fl = floor(p);
    ivec2 i  = ivec2(fl) + cellOfs;
    vec2  f  = p - fl;

    // Quintic fade and its derivative — C2, so the fBm's normal is continuous.
    vec2 u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    vec2 ga = cmGradient(i + ivec2(0, 0));
    vec2 gb = cmGradient(i + ivec2(1, 0));
    vec2 gc = cmGradient(i + ivec2(0, 1));
    vec2 gd = cmGradient(i + ivec2(1, 1));

    float va = dot(ga, f - vec2(0.0, 0.0));
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));

    float k1 = vb - va;
    float k2 = vc - va;
    float k3 = va - vb - vc + vd;

    float value = va + k1 * u.x + k2 * u.y + k3 * u.x * u.y;
    vec2  deriv = ga + u.x * (gb - ga) + u.y * (gc - ga)
                     + u.x * u.y * (ga - gb - gc + gd)
                + du * vec2(k1 + k3 * u.y, k2 + k3 * u.x);
    return vec3(value, deriv);
}

// ---------------------------------------------------------------------------
// Band-passed fBm.
//
// `rel` is CAMERA-RELATIVE world XZ — the coordinate both stages already hold,
// and the one that stays small. `c` is the rendered cell size from cmCellSize.
// `dataFloor` is the finest cell the height pyramid resolves here.
// Returns vec3(metres, dH/dx, dH/dz).
//
// The octave stack is a FIXED lattice of absolute wavelengths, and the two
// limits select a window into it rather than moving it. That distinction is the
// whole design: a start wavelength that slid with the data floor would make the
// noise pattern morph continuously as the fine layer travels with the camera,
// which reads as the ground swimming. Octaves at fixed wavelengths only fade in
// and out.
//
// Precision, concretely. Octaves at or below u_detailWavelength take the
// anchored path: such an octave is alive only where c < 0.6*lambda, and
// c >= dxz * u_invK, so it is evaluated only within ~19 wavelengths of the
// camera. The lattice coordinate there is (|rel| + |u_detailOffset|)/lambda,
// bounded by roughly 19 + detailAnchorStep()/lambda — a few hundred even for
// the finest octave, which fp32 resolves to thousands of steps per lattice
// cell. The world position that would have blown that budget lives in
// `cellOfs`, exactly: detailAnchorStep() is a whole number of base wavelengths
// and every finer octave halves it, so u_detailAnchor/lambda is an exact
// integer in fp32. When the anchor jumps a step, u_detailOffset absorbs the
// same step and the two cancel, so the field does not shift under a moving
// camera. Octaves ABOVE the base wavelength do not divide the anchor step and
// do not need to — see the branch below.
// `ampSum` returns the total amplitude actually summed, which is what acc.x is
// bounded by. The band's top now MOVES with the data, so a caller cannot infer
// the dominant amplitude from u_detailWavelength any more — it has to be
// measured, or anything normalised against it (cavity) saturates at distance.
vec3 cmDetail(vec2 rel, float c, float dataFloor, out float ampSum) {
    ampSum       = 0.0;
    vec2  q      = rel + u_detailOffset;
    vec2  abs_   = rel + u_camXZ;
    // The upward octaves exist only to fill the gap between the data floor and
    // the base wavelength. An exemplar fills that gap with real landforms, so
    // the noise stands down to its original band rather than summing on top of
    // structure that is already there.
    int   up     = (u_exPresent > 0.5) ? 0 : CM_DETAIL_UP_OCTAVES;
    float lambda = u_detailWavelength * exp2(float(up));
    vec3  acc    = vec3(0.0);
    bool  live   = false;

    int n = up + int(u_detailOctaves);
    for (int i = 0; i < CM_DETAIL_MAX_OCTAVES; ++i) {
        if (i >= n) break;
        // LOW-PASS — the pixel. Sampling finer than the framebuffer aliases.
        float wPix = 1.0 - smoothstep(CM_DETAIL_FADE_LO * lambda,
                                      CM_DETAIL_FADE_HI * lambda, c);
        // HIGH-PASS — the data. A grid of cell d represents no wavelength
        // shorter than 2d, so detail owns everything below that and the height
        // pyramid owns everything above. Without this the two overlap wherever
        // the data is fine and, far more visibly, NEITHER covers the decades
        // between a 7.68 km coarse cell and a fixed 48 m start.
        float wDat = 1.0 - smoothstep(2.0 * dataFloor, 4.0 * dataFloor, lambda);
        float w    = wPix * wDat;
        // Coarse→fine, so the live octaves are one contiguous run: the data
        // kills the coarse end, the pixel the fine end. Skip up to the run,
        // stop after it.
        if (w <= 0.0) {
            if (live) break;
            lambda *= 0.5;
            continue;
        }
        live = true;

        // Self-similar at and above the base wavelength; u_detailGain only
        // shapes the decades BELOW it, so widening the band upward leaves the
        // near-ground look exactly as it was tuned.
        float gain = pow(u_detailGain, max(0.0, float(i) - float(up)));

        // Amplitude is PROPORTIONAL TO WAVELENGTH, not an absolute metre
        // count. An absolute amplitude has to be retuned for every world — six
        // metres of relief is a landscape on a gentle island and invisible
        // noise on a 2.6 km mountain range — and it is not even self-consistent
        // across octaves. Tying it to the wavelength makes the detail
        // scale-free: every octave contributes the same slope, so u_detailRelief
        // reads as "how rough" in no units at all, and u_detailGain becomes a
        // deviation from exact self-similarity rather than the whole story.
        float amp = u_detailRelief * lambda * gain;

        float inv = 1.0 / lambda;
        // The anchor split exists to keep the LATTICE COORDINATE small where
        // fp32 would otherwise quantise it — at 100 km a 0.75 m octave gets
        // only ~96 steps per cell. It costs an exact anchor/lambda integer,
        // which holds only for octaves that divide the anchor step. Coarser
        // octaves do not need it: at lambda >= the base wavelength the absolute
        // coordinate still resolves thousands of steps per cell even 500 km
        // out. The two branches agree exactly where both are valid, so the
        // switch introduces no seam.
        vec3 nd = (lambda > u_detailWavelength)
                ? cmNoiseD(abs_ * inv, ivec2(0))
                : cmNoiseD(q * inv, ivec2(u_detailAnchor * inv));
        acc.x  += amp * w * nd.x;
        acc.yz += amp * w * inv * nd.yz;   // chain rule through q * inv
        ampSum += amp * w;

        lambda *= 0.5;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Learned detail exemplar.
//
// A patch of decoder output, high-passed, made periodic and divided by its own
// footprint (see ClipmapTerrain::setDetailExemplar). Because it is stored as
// relief PER UNIT LENGTH, applying it over a repeat of `lambda` metres gives a
// height of lambda * E — the aspect ratio the model produced, at whatever scale
// it is asked for, with no tuned amplitude anywhere.
//
// One tap is a whole octave stack. The patch holds ~900 texels per repeat, so a
// single sample at lambda carries structure from lambda down to lambda/900, and
// the mip chain band-limits the fine end exactly the way the octave loop's
// smoothstep does — that is what a mip chain IS. The gradient costs four more
// taps at an EXPLICIT lod, which is also why manual wrapping is not needed and
// would not work: the sampler wraps (GL_REPEAT), and explicit-lod sampling has
// no implicit derivatives to break at the seam.
// Two taps at an awkward ratio, each rotated differently. One tap alone repeats
// on a visible grid at this scale; a second at a non-integer ratio beats
// against the first with a period neither one has, and the rotation stops the
// pair from sharing a drainage direction across the whole world.
const float CM_EX_RATIO  = 13.7;
const float CM_EX_ROT_A  = 0.31;
const float CM_EX_ROT_B  = 2.24;
const float CM_EX_FINE_W = 0.55;

vec3 cmExemplarTap(vec2 wxz, float lambda, float c, float rot) {
    float cs = cos(rot), sn = sin(rot);
    mat2  R  = mat2(cs, -sn, sn, cs);
    vec2  u  = (R * wxz) / lambda;

    // The patch's texel is lambda/N metres; ask for the level whose texel
    // matches the rendered cell, so the tap is filtered to the same scale
    // everything else at this distance is.
    float texel = lambda / u_exN;
    float lod   = max(0.0, log2(max(c, 1e-6) / texel));
    float step  = exp2(lod) / u_exN;          // one sampled texel, in repeats

    float h  = textureLod(u_exemplar, u, lod).r;
    float hR = textureLod(u_exemplar, u + vec2(step, 0.0), lod).r;
    float hL = textureLod(u_exemplar, u - vec2(step, 0.0), lod).r;
    float hU = textureLod(u_exemplar, u + vec2(0.0, step), lod).r;
    float hD = textureLod(u_exemplar, u - vec2(0.0, step), lod).r;

    // H = lambda * E(u), u = R*w/lambda  =>  dH/dw = R^T * dE/du, so the
    // gradient is scale-free and the lambda cancels exactly.
    vec2 dU = vec2(hR - hL, hU - hD) / (2.0 * step);
    vec2 dW = transpose(R) * dU;
    return vec3(lambda * h, dW);
}

// How much of a tap at `lambda` is NOT already in the data.
//
// The patch is high-passed at about an eighth of its footprint, so a tap at
// repeat `lambda` has its coarsest content near lambda/8; the pyramid holds
// nothing below 2*dataFloor. Where the patch's content is entirely above that,
// the data already says it and the tap fades out.
float cmExRedundancy(float lambda, float dataFloor) {
    return 1.0 - smoothstep(2.0 * dataFloor, 8.0 * dataFloor, lambda * 0.125);
}

// Detail from the exemplar at a point. `c` is the cell this stage is limited by
// and `dataFloor` the finest cell the height pyramid resolves here.
//
// The repeat lengths are FIXED — tied to the coarsest layer, uniform across the
// world — and redundancy against the data is expressed as AMPLITUDE. Scaling
// the wavelength by the local data floor instead would look equivalent and is
// not: the floor moves as the fine layer travels with the camera, so the patch
// would stretch and the structure at a fixed world point would morph
// continuously. Same reason the noise octaves sit on a fixed lattice.
//
// NOT modulated by the ground's slope, unlike the noise: the patch is real
// terrain and already carries its own flat reaches and broken faces. Scaling it
// by the coarse field's slope would double-count the landscape's own roughness
// and flatten exactly the distant ground this exists to give relief to.
vec3 cmExemplar(vec2 rel, float c, float dataFloor) {
    if (u_exPresent < 0.5) return vec3(0.0);
    vec2 w = rel + u_camXZ;

    float la = u_exLambda;
    float lb = la / CM_EX_RATIO;
    float wa = cmExRedundancy(la, dataFloor);
    float wb = cmExRedundancy(lb, dataFloor) * CM_EX_FINE_W;

    vec3 acc = vec3(0.0);
    if (wa > 0.0) acc += wa * cmExemplarTap(w, la, c, CM_EX_ROT_A);
    if (wb > 0.0) acc += wb * cmExemplarTap(w, lb, c, CM_EX_ROT_B);
    return acc;
}

// How much detail this part of the world wants.
//
// PROPORTIONAL to the ground's own slope, not a ramp between two constants:
// flat ground is flat — a meadow has no metre-scale rock relief — while a
// mountainside is broken rock all the way down, and the steeper it is the more
// broken. Proportionality also carries cmDetail's scale-free property through:
// a world with ten times the relief gets ten times the detail without anyone
// touching a setting.
//
// The floor keeps plains from going glassy. The modulator varies over hundreds
// of metres against detail measured in metres, so its own gradient is dropped
// from the normal; the error is far below the detail's own slope.
float cmDetailWeight(float slope) {
    return max(slope, CM_DETAIL_FLOOR);
}
