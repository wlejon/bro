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

const int CM_DETAIL_MAX_OCTAVES = 8;

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
// Band-limited fBm.
//
// `rel` is CAMERA-RELATIVE world XZ — the coordinate both stages already hold,
// and the one that stays small. `c` is the rendered cell size from cmCellSize.
// Returns vec3(metres, dH/dx, dH/dz).
//
// Precision, concretely: an octave is alive only where c < 0.6*lambda, and
// c >= dxz * u_invK, so it is evaluated only within ~19 wavelengths of the
// camera. The lattice coordinate there is (|rel| + |u_detailOffset|)/lambda,
// bounded by roughly 19 + kDetailAnchor/lambda — a few hundred even for the
// finest octave, which fp32 resolves to thousands of steps per lattice cell.
// The world position that would have blown that budget lives in `cellOfs`,
// exactly: u_detailAnchor is a multiple of kDetailAnchor and every
// wavelength divides it, so u_detailAnchor/lambda is an exact integer in fp32.
// When the anchor jumps a step, u_detailOffset absorbs the same step and the
// two cancel, so the field does not shift under a moving camera.
vec3 cmDetail(vec2 rel, float c) {
    vec2  q      = rel + u_detailOffset;
    float lambda = u_detailWavelength;
    vec3  acc    = vec3(0.0);
    float gain   = 1.0;

    int n = int(u_detailOctaves);
    for (int i = 0; i < CM_DETAIL_MAX_OCTAVES; ++i) {
        if (i >= n) break;
        // Monotonic in i: coarser octaves have a wider window, so once one
        // octave is fully faded every finer one is too.
        float w = 1.0 - smoothstep(CM_DETAIL_FADE_LO * lambda,
                                   CM_DETAIL_FADE_HI * lambda, c);
        if (w <= 0.0) break;

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
        vec3  nd  = cmNoiseD(q * inv, ivec2(u_detailAnchor * inv));
        acc.x += amp * w * nd.x;
        acc.yz += amp * w * inv * nd.yz;   // chain rule through q * inv

        lambda *= 0.5;
        gain   *= u_detailGain;
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
