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

// Roughening a SMOOTH learned layer.
//
// The data high-pass below assumes the height pyramid is band-limited: a grid of
// cell d carries every wavelength down to 2d, so detail owns only what is finer.
// That holds for the coarse chart. It does NOT hold for the streamed 30 m
// decoder layer — that is a smooth heightfield that carries a mountain's macro
// SHAPE but none of its sub-kilometre ruggedness, so trusting it down to 2*30 m
// leaves the mountainside glassy (the "airplane view" look). Where the data
// floor is finer than CM_ROUGHEN_DATA_M we therefore stop trusting the data at
// 2*floor and instead let procedural detail fill down from CM_ROUGHEN_M — adding
// the rock the smooth layer lacks. It is still slope-keyed by cmDetailWeight, so
// it lands on the steep faces the decoder smoothed and leaves plains alone, and
// the coarse-only world (data floor kilometres wide) is completely unaffected.
const float CM_ROUGHEN_DATA_M = 400.0;   // a data floor finer than this = smooth learned window
const float CM_ROUGHEN_M      = 500.0;   // procedural fills the surface from ~2x this down

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
    uint h = cmHashU(c);
    const vec2 grads[8] = vec2[](
        vec2( 1.0,  0.0),
        vec2( 0.70710678,  0.70710678),
        vec2( 0.0,  1.0),
        vec2(-0.70710678,  0.70710678),
        vec2(-1.0,  0.0),
        vec2(-0.70710678, -0.70710678),
        vec2( 0.0, -1.0),
        vec2( 0.70710678, -0.70710678)
    );
    return grads[h & 7u];
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
    // The upward octaves fill the gap between the data floor and the base
    // wavelength: wherever the finest data underfoot is coarser than the base
    // wavelength, the band climbs above it so the surface reads as terrain
    // rather than a smooth ramp under the coarse cell.
    int   up     = CM_DETAIL_UP_OCTAVES;
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
        //
        // EXCEPT over a smooth learned window (see CM_ROUGHEN_*): there the data
        // floor is metres but the data is glassy below the kilometre, so trusting
        // it that far leaves no ruggedness. Roughen from a fixed coarser ceiling
        // instead, letting procedural overlap the band the decoder rendered flat.
        float hpFloor = (dataFloor < CM_ROUGHEN_DATA_M) ? CM_ROUGHEN_M : dataFloor;
        float wDat = 1.0 - smoothstep(2.0 * hpFloor, 4.0 * hpFloor, lambda);
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
