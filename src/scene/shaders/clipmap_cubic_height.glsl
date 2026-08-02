// Clipmap terrain — MIP-AWARE CUBIC RECONSTRUCTION OF THE HEIGHT FIELD (opt-in).
//
// This chunk is concatenated into BOTH the vertex and the fragment source, and
// only when ClipmapConfig::cubicHeight is on. With the flag off it is not
// appended at all, so the source the driver compiles is byte-for-byte the
// source it compiled before this file existed.
//
// BOTH stages, deliberately. cmHeight displaces the geometry in the vertex
// stage and is read five more times per fragment to build the shading normal.
// A reconstruction applied to one of those and not the other would shade a
// surface the mesh does not describe. This is the lesson the consumer paid for
// downstream: a private filtered ALTITUDE read placed beside an unfiltered
// rendered height measured MORE speckle, not less (band energy 5.47 against
// 4.69 with no filter at all), because a threshold on one field drawn over
// another field is incoherent at every pixel where they differ. The filter has
// to be the surface, or it is noise.
//
// ---------------------------------------------------------------------------
// WHY. Same argument as clipmap_cubic.glsl makes for the control channels, one
// field over: GL_LINEAR reconstructs a height layer as a piecewise-BILINEAR
// surface, C0, with a derivative that jumps at every texel edge. A THRESHOLD on
// altitude — a snow line, a shoreline, a facies cut — therefore draws its
// contour as a chain of straight segments hinged on the data lattice. The level
// set of a bilinear patch is a hyperbola INSIDE each texel and kinks at the
// boundary, so the artefact's visible period is one texel:
//
//     one texel spans N pixels  =>  the contour hinges every N pixels
//
// which is why this reads as jagged, stair-stepped crescents from orbit (N a
// few pixels) and as nothing at all underfoot (N in the thousands — you are
// looking at one smooth hyperbola arc). That single relation is the whole
// design: it says where the fix is needed, and it says where it must not cost
// anything. See THE GATE below.
//
// It is not only the contour. The five-tap shading normal differences this same
// field at spacing e = max(c, dataFloor), which at a coarse rung IS one texel,
// so a C0 height gives a normal with a jump on the lattice — flat-shaded texel
// facets with hard edges, the same grid in the light instead of in the colour.
// Both go away together, because both are the same discontinuity.
//
// ---------------------------------------------------------------------------
// THE FILTER: CUBIC B-SPLINE, exactly as clipmap_cubic.glsl derives it. For a
// fraction f in [0,1) between texels i and i+1, taps at i-1..i+2:
//     w0 = (1-f)^3 / 6,  w1 = (3f^3 - 6f^2 + 4)/6,
//     w2 = (-3f^3 + 3f^2 + 3f + 1)/6,  w3 = f^3/6,   sum = 1 identically,
// every weight >= 0, so the reconstruction is bounded by the data it reads: no
// overshoot, no ringing halo beside a contour, and the existing cull margin
// (built from minHeight_/maxHeight_) still bounds the displacement. Catmull-Rom
// interpolates but has negative outer weights and cannot use the tap reduction
// — the full argument is in clipmap_cubic.glsl and is not repeated here.
//
// The price is the same too: a B-spline APPROXIMATES. An isolated one-texel
// spike reads 4/6 of its stored height. That is a real geometric change, and it
// is the second reason the gate below matters — near the camera, where a texel
// is the picture, one third of a peak is metres of mountain; at a rung whose
// texel is four pixels wide it is invisible.
//
// ---------------------------------------------------------------------------
// THE MIP PROBLEM, which is what made this a separate change from the control
// channels. cmLayer does not sample level 0. It samples a FRACTIONAL mip:
//
//     lod = max(log2(cDesired / metresPerCell), 0)
//
// and at lod > 0 the field the pixel actually sees is the level-lod field,
// whose texels are 2^lod times wider. A filter whose taps are spaced at LEVEL
// ZERO's texel size then places all four fetches inside a single sampled texel,
// where the hardware's own bilinear is already exactly linear — the four taps
// sum to a convex combination of one bilinear patch and the result is PLAIN
// TRILINEAR. It would cost four fetches and change nothing, and it would do
// that at precisely the coarse rungs it was added for. So the taps are spaced
// at the SAMPLED level's texel size, and the fetches are taken AT that level.
//
// Derivation of the level-M texel grid. uv is level-independent, and level M
// has size sM = max(1, floor(size0 / 2^M)) texels (GL's own rule, floor and
// clamp, so this is exact for a non-power-of-two layer too). A texel coordinate
// at that level is
//
//     tM = uv * sM - 0.5
//
// and everything else — floor, fraction, the four weights, the two paired
// fetch placements — is the level-0 derivation with sM in place of size0 and
// textureLod(..., M) in place of texture(). Two bilinear fetches per axis,
// four in 2D, the filter unit delivering each pair's weights exactly, and the
// two half-sums s0, s1 bounded below by 1/6 for every fraction so neither
// placement divides by anything near zero.
//
// ---------------------------------------------------------------------------
// THE FRACTION. lod is fractional, and a filter has to answer for the part
// between two levels. Two candidates, and the choice is not close:
//
//   (A) FILTER AT BOTH BRACKETING LEVELS AND BLEND (what this file does).
//       Evaluate the cubic at M = floor(lod) and at M+1, blend by the
//       fraction. Eight fetches. It is the cubic analogue of what trilinear
//       already does, so it inherits trilinear's continuity in scale, and
//       within either level it is the exact C2 reconstruction.
//
//   (B) FILTER AT ONE RESOLVED LEVEL — round lod, or floor it, and take four
//       fetches there. Half the cost, and rejected: the reconstruction then
//       JUMPS by cubic_M - cubic_{M+1} wherever the rounding changes, which is
//       the full difference between two mip levels — the local high-frequency
//       energy of the height field at that scale, metres to tens of metres on
//       real terrain at a coarse rung. Worse, lod is a smooth function of
//       distance, so that jump lands on a smooth camera-centred surface and
//       renders as a hard terrace RING around the eye, in the geometry and in
//       the shading at once. It would reintroduce, in the name of removing a
//       discontinuity, exactly the LOD pop that the fractional mip exists to
//       remove. The error is not small and it is not subtle; it is the reason
//       the eight fetches are worth paying where they are paid at all.
//
//   A third option — four fetches spaced at level lod but taken with a
//   FRACTIONAL textureLod — is worth naming because it looks like it works and
//   does not. Each fetch then returns a trilinear blend of two levels, and the
//   pair-weight placement is exact for NEITHER of them (the placement offset is
//   in level-lod texels; the hardware lerps between adjacent level-M texels, a
//   different grid). What comes back is a fixed linear combination of four C0
//   trilinear samples, which is C0 — smoother than one sample, and still not
//   C1. This filter exists to be C1. Rejected on that alone.
//
// C1 IN SCALE AS WELL AS IN SPACE. The blend weight is smoothstep(fr), not fr.
// A linear blend of the two levels is continuous in value but not in gradient:
// crossing an integer lod, the spatial gradient jumps by
//     (cubic_{M+1} - 2*cubic_M + cubic_{M-1}) * grad(lod)
// — the second difference ACROSS mip levels, times the gradient of lod. It is
// small (lod changes by 1 over a doubling of distance) and it is exactly what
// hardware trilinear already does, but it is free to remove: smoothstep has
// zero derivative at both ends, so the grad(lod) term vanishes at the crossing
// and the reconstruction is C1 in scale as well as C2 in space. The weights
// stay a convex combination, so nothing about the no-overshoot argument moves.
//
// WHAT REMAINS DISCONTINUOUS, stated plainly rather than claimed away: the
// gate's own blend (below) rides on cmCellSizeAA, whose Chebyshev max has
// gradient kinks along the diagonals — but cmCellSize feeds the mip selection
// and the detail band limit through the same max, so those kink surfaces
// already exist in the rendered field and this adds no new ones. What is
// removed is the whole family of C0 kinks on the TEXEL LATTICE, which is the
// family a threshold turns into straight segments.
//
// ---------------------------------------------------------------------------
// THE GATE — how the cost is bounded, and why it is a property of the RUNG and
// not of the camera.
//
// From the relation at the top: the artefact's period is one sampled texel, so
// it is visible only while a sampled texel is a handful of pixels across. Write
// that quantity down. The sampled texel is max(metresPerCell, cDesired) metres
// (lod is clamped at 0, so below cDesired the layer is still read at level 0),
// and one pixel is cmCellSizeAA / CM_SHADE_PIXELS_PER_CELL metres:
//
//     texelPixels = max(T, c) / (dist * u_pixelScale)
//
// and the cubic path fades out across CM_CUBIC_PX_ON..CM_CUBIC_PX_OFF pixels.
// A contour hinged every 32 px reads as a chain of straight segments; by 128 px
// the hyperbola inside one texel is itself visibly curved and the hinge between
// two of them is not findable. Both ends matter: below the band the filter is
// needed, above it there is nothing to fix AND the B-spline's one-third
// flattening of a peak would be a visible loss instead of a sub-pixel one.
//
// The lower end never binds, by construction: c >= cAA = 1.5 px and the sampled
// texel is at least c, so texelPixels >= 1.5 everywhere. There is no regime
// where the mip has already made the lattice sub-pixel.
//
// WHERE THAT PUTS THE COST. In the geometry-limited far field (where the ring
// cell is the coarse thing, cGeo = dxz * u_invK, and dist ~ dxz) the ratio
// collapses to a CONSTANT:
//
//     texelPixels -> 1 / (u_invK * u_pixelScale)
//
// — 8 px at resolution 512 and 1080p, 33 px at resolution 128 — independent of
// altitude. So the cubic path is keyed to the RUNG: a fragment pays exactly
// when the rung it reads from is drawing texels a few pixels wide, which is the
// far field at any altitude and the whole frame from orbit. Underfoot, where
// dxz is small and the layer's own texel dominates (texelPixels in the
// thousands), it pays nothing at all and the height is the bilinear fetch it
// always was, to the bit. That is also what keeps the CPU mirrors honest — see
// the note at the end of this file.
//
// AND layerFade DOES THE REST. A layer whose data has gone deeply sub-pixel is
// mixed in with an exact 0.0 (cmLayerFade), and a layer outside its own
// footprint with an exact 0.0 coverage weight. Either way mix(h, s, 0.0) is h
// to the bit, so the chain below computes those weights FIRST — both are pure
// arithmetic, cmCoverage takes no fetch — and skips the reconstruction whose
// result it would discard. Bit-identical to computing it, and it is what bounds
// the orbital case: two or three layers live out of six, four fetches where lod
// is clamped at 0 and eight where it is not, instead of six layers times eight
// unconditionally.
//
// ---------------------------------------------------------------------------
// EDGES. The four fetches reach from texel i-1 to i+2 at the sampled level, up
// to two texels outside the [0.5, sM-0.5] band of texel centres, and are
// CLAMPED into it one axis at a time — except in X on a layer marked wrapX,
// where there is no edge to clamp to and GL_REPEAT resolves the coordinate
// across mip levels exactly as it does for cmLayer's single fetch. So this path
// reads no texel the bilinear path could not, on either kind of layer. Inside
// the last texel the filter degrades continuously toward the border value; it
// never fetches outside the layer and cannot invent data beyond it. That
// degradation is an order of magnitude inside the coverage ramp that is already
// fading the layer out (CM_FADE = 8% of the layer per axis, so 0.08*W texels),
// which keeps the two-texel reach inside the ramp for any layer wider than 25
// texels.
//
// ---------------------------------------------------------------------------
// THE CPU MIRRORS. baseElevationAt / elevationAt / renderedElevationAt are
// UNCHANGED by this mode, and that is the coherent answer rather than an
// omission. They are camera-free — a collision query whose answer moved when
// the camera moved would be a worse bug than any filter fixes — and the gate
// above is exactly 0 in the region they are exact in: near the camera, where
// the mirrors already match the GPU because lod is 0 and layerFade is 1. So in
// BOTH modes the standable ground is the drawn ground there, to the bit, and
// the suite asserts it as frame equality.
//
// Where they diverge, bounded rather than left implicit: past the gate's ramp
// the drawn sheet is the cubic reconstruction and the mirror reports the
// bilinear one. The difference is at most (1/6) of the field's second
// difference over one sampled texel, and 1/3 of the height of an isolated
// one-texel spike. That is the same order as — and lands in the same place as —
// the mip divergence the mirror already carries (it samples level 0 where the
// GPU is at level lod), which is documented on elevationAt(). The mirror cannot
// model mips at all: there is no CPU mip chain, and building one would be a
// second copy of the height pyramid to keep in step. Nothing stands on ground
// that far from the camera; a camera grounded from orbit is placed against a
// sheet that has moved by tens of metres under a 600 km eye.
//
// NOT IN SCOPE: cmDataFloor, which takes no texel reads (it blends per-layer
// log2(metresPerCell) constants by coverage weights), and the control channels,
// which have their own opt-in filter in clipmap_cubic.glsl and are sampled at
// level 0 by construction.

// Pixels one SAMPLED texel of a layer with texel size `texel` spans at `wxz`.
// The artefact's period, in the units the eye judges it in.
float cmTexelPixels(vec2 wxz, float texel, float cDesired) {
    // cmCellSizeAA is CM_SHADE_PIXELS_PER_CELL pixels' worth of ground, so
    // dividing it out leaves exactly one pixel, in metres, at this point.
    float px = cmCellSizeAA(wxz) / CM_SHADE_PIXELS_PER_CELL;
    return max(texel, cDesired) / max(px, 1e-6);
}

// Fully cubic at or below CM_CUBIC_PX_ON pixels per sampled texel, fully
// bilinear at or above CM_CUBIC_PX_OFF. See THE GATE above for both numbers.
const float CM_CUBIC_PX_ON  = 32.0;
const float CM_CUBIC_PX_OFF = 128.0;

// One cubic-B-spline sample of a MIPMAPPED texture AT INTEGER LEVEL `lvl`, in
// four bilinear fetches taken at that level. `size0` is the level-0 size in
// texels; the level's own size is derived by GL's rule. `wrapX` > 0.5 leaves
// the X coordinate unclamped for GL_REPEAT to resolve.
//
// Public: an app composing its own material chunk, or thresholding on a
// channel of its own, can put that read through this and get the same surface
// the sheet is drawn from.
vec4 cmCubicTapLevel(sampler2D tex, vec2 uv, vec2 size0, float lvl, float wrapX) {
    vec2 sz = max(floor(size0 * exp2(-lvl)), vec2(1.0));

    vec2 tc = uv * sz - 0.5;
    vec2 f  = fract(tc);
    tc = floor(tc);

    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
    vec2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    vec2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    vec2 w3 = f3 / 6.0;

    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;
    vec2 t0 = (tc - 1.0 + w1 / s0 + 0.5) / sz;
    vec2 t1 = (tc + 1.0 + w3 / s1 + 0.5) / sz;

    // Clamped into the band of texel centres AT THIS LEVEL. X is left alone on
    // a periodic layer — there is no east-west edge there, and cmLayer relies
    // on GL_REPEAT resolving it across levels for the same reason.
    vec2 lo = 0.5 / sz;
    vec2 hi = 1.0 - lo;
    vec2 c0 = clamp(t0, lo, hi);
    vec2 c1 = clamp(t1, lo, hi);
    t0 = vec2((wrapX > 0.5) ? t0.x : c0.x, c0.y);
    t1 = vec2((wrapX > 0.5) ? t1.x : c1.x, c1.y);

    return mix(mix(textureLod(tex, vec2(t0.x, t0.y), lvl),
                   textureLod(tex, vec2(t1.x, t0.y), lvl), s1.x),
               mix(textureLod(tex, vec2(t0.x, t1.y), lvl),
                   textureLod(tex, vec2(t1.x, t1.y), lvl), s1.x), s1.y);
}

// The same at a FRACTIONAL level: the cubic at both bracketing levels, blended
// by smoothstep of the fraction so the result is C1 across the crossing as well
// as C2 within a level. At lod 0 — which is the whole of the near field, and
// the coarsest layer at orbital altitudes where the pixel is still finer than
// its texel — the fraction is an exact 0 and this is four fetches, not eight.
float cmCubicHeightAt(sampler2D tex, vec2 uv, vec2 size0, float lod, float wrapX) {
    float m0 = floor(lod);
    float fr = lod - m0;
    float a  = cmCubicTapLevel(tex, uv, size0, m0, wrapX).r;
    if (fr <= 0.0) return a;
    float b  = cmCubicTapLevel(tex, uv, size0, m0 + 1.0, wrapX).r;
    return mix(a, b, fr * fr * (3.0 - 2.0 * fr));
}

// cmLayer with the single trilinear fetch replaced by the gated, mip-aware
// cubic. uv, lod and the coverage weight are computed by the IDENTICAL
// expressions cmLayer uses, so where the gate is 0 this returns cmLayer's own
// value to the bit — the off-regime is not an approximation of the old path,
// it is the old path.
float cmLayerCubic(sampler2D tex, vec3 a, vec2 sz, vec2 wxz, float cDesired,
                   float wrapX, out float w) {
    if (sz.x < 0.5 || sz.y < 0.5) { w = 0.0; return 0.0; }
    vec2 t  = (wxz - a.xy) / a.z;
    vec2 uv = (t + 0.5) / sz;
    float lod = max(log2(cDesired / a.z), 0.0);
    w = smoothstep(0.0, CM_FADE, cmEdge(uv, wrapX));

    float g = 1.0 - smoothstep(CM_CUBIC_PX_ON, CM_CUBIC_PX_OFF,
                               cmTexelPixels(wxz, a.z, cDesired));
    if (g <= 0.0) return textureLod(tex, uv, lod).r;
    float cub = cmCubicHeightAt(tex, uv, sz, lod, wrapX);
    if (g >= 1.0) return cub;
    return mix(textureLod(tex, uv, lod).r, cub, g);
}

// cmHeight's chain, layer for layer and mix for mix, reading through the cubic
// filter. Kept structurally identical on purpose: same coarsest-first order,
// same coverage weights, same cmLayerFade factors, same mix() calls in the same
// sequence, so the height stack still agrees with cmDataFloor and cmSurface
// about which layers are live and where a layer stops.
//
// The one addition is the skip. A finer layer enters through mix(h, s, w * fd);
// both factors are computed here without a texture fetch (cmCoverage is the
// same ramp cmLayerCubic's own `w` comes out as, and cmLayerFade is arithmetic
// on two scalars), so when their product is an exact 0.0 the mix would return h
// unchanged and the reconstruction is skipped instead of computed and thrown
// away. That is bit-identical by the same argument layerFade's exact zero
// rests on, and it is what keeps an orbital fragment paying for the two or
// three layers that are live rather than for all six.
float cmHeightCubic(vec2 wxz, float cDesired) {
    float n = u_layerCount;
    float w = 0.0;
    float h = 0.0;
    if      (n > 5.5) h = cmLayerCubic(u_h5, u_l5a, u_l5b, wxz, cDesired, u_lWrapX45.y, w);
    else if (n > 4.5) h = cmLayerCubic(u_h4, u_l4a, u_l4b, wxz, cDesired, u_lWrapX45.x, w);
    else if (n > 3.5) h = cmLayerCubic(u_h3, u_l3a, u_l3b, wxz, cDesired, u_lWrapX.w, w);
    else if (n > 2.5) h = cmLayerCubic(u_h2, u_l2a, u_l2b, wxz, cDesired, u_lWrapX.z, w);
    else if (n > 1.5) h = cmLayerCubic(u_h1, u_l1a, u_l1b, wxz, cDesired, u_lWrapX.y, w);
    else if (n > 0.5) h = cmLayerCubic(u_h0, u_l0a, u_l0b, wxz, cDesired, u_lWrapX.x, w);
    if (n > 5.5 && cmCoverage(u_l4a, u_l4b, wxz, u_lWrapX45.x) * cmLayerFade(u_l4a.z, cDesired) > 0.0) { float s = cmLayerCubic(u_h4, u_l4a, u_l4b, wxz, cDesired, u_lWrapX45.x, w); h = mix(h, s, w * cmLayerFade(u_l4a.z, cDesired)); }
    if (n > 4.5 && cmCoverage(u_l3a, u_l3b, wxz, u_lWrapX.w) * cmLayerFade(u_l3a.z, cDesired) > 0.0) { float s = cmLayerCubic(u_h3, u_l3a, u_l3b, wxz, cDesired, u_lWrapX.w, w); h = mix(h, s, w * cmLayerFade(u_l3a.z, cDesired)); }
    if (n > 3.5 && cmCoverage(u_l2a, u_l2b, wxz, u_lWrapX.z) * cmLayerFade(u_l2a.z, cDesired) > 0.0) { float s = cmLayerCubic(u_h2, u_l2a, u_l2b, wxz, cDesired, u_lWrapX.z, w); h = mix(h, s, w * cmLayerFade(u_l2a.z, cDesired)); }
    if (n > 2.5 && cmCoverage(u_l1a, u_l1b, wxz, u_lWrapX.y) * cmLayerFade(u_l1a.z, cDesired) > 0.0) { float s = cmLayerCubic(u_h1, u_l1a, u_l1b, wxz, cDesired, u_lWrapX.y, w); h = mix(h, s, w * cmLayerFade(u_l1a.z, cDesired)); }
    if (n > 1.5 && cmCoverage(u_l0a, u_l0b, wxz, u_lWrapX.x) * cmLayerFade(u_l0a.z, cDesired) > 0.0) { float s = cmLayerCubic(u_h0, u_l0a, u_l0b, wxz, cDesired, u_lWrapX.x, w); h = mix(h, s, w * cmLayerFade(u_l0a.z, cDesired)); }
    return u_seaLevel + u_heightScale * h;
}

// The switch. Object-like, so it renames the IDENTIFIER at every call site
// below — the vertex stage's own cmSurface(), its four morph taps, and the
// fragment stage's five normal taps — without any of them changing by a byte.
// It lands after cmHeight's definition in clipmap_common.glsl and before every
// caller, in both stages, which is what makes the drawn ground and the shaded
// ground the same surface in this mode as in the other.
#define cmHeight cmHeightCubic
