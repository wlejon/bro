// Clipmap terrain — CUBIC RECONSTRUCTION OF THE CONTROL CHANNELS (opt-in).
//
// This chunk is concatenated into the FRAGMENT source only, and only when
// ClipmapConfig::cubicSurface is on. With the flag off it is not appended at
// all, so the source the driver compiles is byte-for-byte the source it
// compiled before this file existed — the off path cannot be shifted by dead
// code, because there is no code.
//
// ---------------------------------------------------------------------------
// WHY. GL_LINEAR reconstructs a control channel as a piecewise-BILINEAR
// surface: continuous, but with a derivative that jumps at every texel edge.
// Nothing downstream cares until a THRESHOLD is put on it — and every consumer
// of these channels puts one on it. cmMaterialAt alone tests `b == 5.0`,
// `b == 2.0` and the band `bf > 3.5 && bf < 4.5`; apps add snow lines,
// shorelines and facies cuts. The level set of a bilinear patch is a hyperbola
// inside each texel with a KINK at the boundary, so a threshold over the
// reconstruction draws its contour as a chain of straight segments hinged on
// the texel lattice: straight-edged vegetation patches, stair-stepped margins,
// diamond fringes — an organic feature bounded by the data grid. It is worst
// where the texel is largest, which is the coarse rungs of the stack, which is
// exactly where a whole planet is drawn from.
//
// Widening the threshold band does not fix it. That draws the SAME polygon,
// fuzzier. The fix has to be the reconstruction, and it is then
// resolution-independent: a C2 field has smooth level sets at any zoom.
//
// ---------------------------------------------------------------------------
// THE FILTER: CUBIC B-SPLINE (Mitchell-Netravali B=1, C=0). Derived here
// before it was coded.
//
// For a fraction f in [0,1) between texels i and i+1, taps at i-1..i+2:
//     w0 = (1-f)^3 / 6        = (-f^3 + 3f^2 - 3f + 1) / 6
//     w1 = (3f^3 - 6f^2 + 4)  / 6
//     w2 = (-3f^3 + 3f^2 + 3f + 1) / 6
//     w3 = f^3 / 6
// Sum = 1 identically. At f = 0 the weights are (1,4,1,0)/6, at f = 0.5 they
// are (0.125, 2.875, 2.875, 0.125)/6 — symmetric, and every weight is >= 0
// across the whole interval.
//
// WHY NOT CATMULL-ROM, which is the other obvious choice. Catmull-Rom
// (B=0, C=0.5) INTERPOLATES — it passes through the texel values, which sounds
// strictly better — but its outer weights are NEGATIVE. Two consequences, and
// both of them matter here:
//   * It overshoots. A channel the app clamped to [0,1] reconstructs above 1
//     next to a peak, so a threshold placed AT the top of the range fires in a
//     halo around every spike. Ringing next to a contour is the artefact this
//     file exists to remove, in a new costume.
//   * It cannot use the tap reduction below. That reduction needs each PAIR of
//     weights to be non-negative with a positive sum, so the hardware's own
//     lerp can deliver the pair. For Catmull-Rom s0 = w0 + w1 falls to exactly
//     0 as f -> 1 and the tap placement w1/s0 divides by it. Sixteen taps per
//     layer per fragment is not a filter, it is a budget.
// The B-spline's weights are a convex combination, so the reconstruction is
// bounded by the data it reads: no ringing, no halo, nothing to clamp.
//
// The price, stated plainly: a B-spline APPROXIMATES rather than interpolates.
// At a texel centre (f = 0) it returns (a + 4b + c)/6, so an isolated
// one-texel spike reads 4/6 = 2/3 of its stored value and a step across one
// texel is spread over about two. Thresholds are therefore set against the
// SMOOTHED channel, not against the deciles of the stored field. That is a
// documented change in what a fragment sees — it is the whole point of asking
// for this mode — and it is why the mode is opt-in rather than the default.
//
// ---------------------------------------------------------------------------
// THE TAP REDUCTION: 16 taps -> 4. Also derived before it was coded.
//
// Separably, a bicubic is 4x4 = 16 point samples. Group the taps in pairs,
// (0,1) and (2,3), and let s0 = w0 + w1, s1 = w2 + w3, so s0 + s1 = 1.
// A hardware BILINEAR fetch at texel coordinate x returns
//     (1 - frac(x)) * T[floor(x)] + frac(x) * T[floor(x) + 1],
// so placing one fetch at
//     xA = (i - 1) + w1/s0   returns (w0*T[i-1] + w1*T[i]) / s0
// and another at
//     xB = (i + 1) + w3/s1   returns (w2*T[i+1] + w3*T[i+2]) / s1.
// Then s0*A + s1*B is the full four-tap sum, and because s0 = 1 - s1 that is
// exactly mix(A, B, s1). One axis: 2 fetches instead of 4. Both axes: 4
// instead of 16. The hardware's filter unit does the other twelve multiplies.
//
// The division is always safe, which is the property Catmull-Rom lacks:
//     s0 = (2f^3 - 3f^2 - 3f + 5) / 6,  ds0/df = (6f^2 - 6f - 3)/6 < 0 on [0,1]
// so s0 falls monotonically from 5/6 at f = 0 to 1/6 at f = 1 and s1 = 1 - s0
// is likewise in [1/6, 5/6]. Both are bounded away from zero by 1/6 for every
// possible fraction. No guard, no epsilon, no special case.
//
// COST. Four fetches per layer where the bilinear chain took one, on textures
// that are already resident and whose four taps land within two texels of each
// other — the same cache lines. The chain below is evaluated ONCE per
// fragment, so the whole mode costs (4 - 1) x layers extra fetches per pixel:
// 18 at a full six-layer stack, against the ~30 the shading normal's five
// cmHeight calls already spend. It is deliberately NOT applied to the height
// pyramid — see the note at the end of this file.
//
// ---------------------------------------------------------------------------
// EDGE BEHAVIOUR. The four bilinear fetches reach from texel i-1 to i+2, up to
// two texels outside the [0.5, size-0.5] band of texel centres. They are
// CLAMPED into that band, one coordinate at a time — exactly the clamp
// cmSurfLayer already applies to its single fetch, so this path can read no
// texel the bilinear path could not. Inside the last texel the filter degrades
// continuously toward the border value; it never fetches outside the layer and
// so cannot invent data beyond it.
//
// That degradation cannot become a border smear, because it is an order of
// magnitude inside the ramp that is already fading the layer out: the coverage
// weight w falls to zero across the outer CM_FADE = 8% of the layer per axis,
// which is 0.08*W texels. The cubic's two-texel reach stays inside that ramp
// for any layer wider than 25 texels, and a control layer narrower than that
// describes a region smaller than the ramp itself. The clamp is the honest
// answer for the remainder: outside the data there is no control state, and
// the ramp is what says so.
//
// ---------------------------------------------------------------------------
// THE LAYER BLEND. Cubic reconstruction is applied PER LAYER, before the mix —
// smoothing each layer's own field, never the blended result — so the chain
// below is cmSurface's chain with one word changed. Same coarsest-first order,
// same coverage weights, same cmLayerFade factor, same mix() calls in the same
// sequence. The exact-zero property layerFade relies on therefore survives
// unaltered: a faded-out layer's weight is an exact 0.0, mix(s, f, 0.0) is s
// to the bit, and the clamped fetches guarantee f is finite so the 0.0 cannot
// be poisoned by a NaN arriving from off the texture.
//
// ---------------------------------------------------------------------------
// HOW IT IS SWITCHED IN. GLSL 330 has no #include; composition plus #define is
// the mechanism the language actually offers, and ClipmapTerrain::ensureNode
// splices this file between the shared chunks and the material chunk. The
// object-like #define at the bottom therefore lands AFTER cmSurface's own
// definition (in clipmap_common.glsl) and BEFORE its only call site (in
// clipmap_material.glsl, or in whatever material chunk an app composes in its
// place), which is what lets the call site switch without either file changing
// by one byte. The vertex source never receives this chunk, so the `cmSurface`
// that clipmap.vert.glsl declares for its own sheet height is untouched.
//
// WHAT THIS DELIBERATELY DOES NOT TOUCH:
//   * The HEIGHT pyramid. cmHeight displaces the drawn geometry, feeds the
//     five-tap shading normal, and is mirrored on the CPU by elevationAt() /
//     renderedElevationAt() for collision and camera grounding. Reconstructing
//     it cubically would move the drawn ground away from the standable ground
//     unless all three mirrors moved with it — and it is not the same filter
//     problem either, because cmLayer reads a FRACTIONAL MIP: correct tap
//     spacing there is the sampled level's texel size, not level 0's, so a
//     level-0 filter would collapse to plain trilinear at exactly the coarse
//     rungs it was added for. That is a separate change with its own evidence
//     to produce. An app whose material thresholds on ALTITUDE can put its own
//     height read through cmCubicTap — the primitive is public here for
//     exactly that — and then it owns the decision to shade against a surface
//     that is not quite the one it draws.
//   * cmDataFloor. It takes no texel reads at all: it blends per-layer
//     log2(metresPerCell) CONSTANTS by the same coverage weights. There is no
//     reconstruction in it to make continuous.

// One cubic-B-spline sample of a NON-MIPMAPPED texture, in four bilinear
// fetches. `size` is the texture's dimensions in texels. Public: an app
// composing its own material chunk may use it on any channel it thresholds.
//
// Do not hand this a texture whose sampler will minify through a mip chain —
// the tap spacing below is level 0's, and at lod > 0 all four fetches land
// inside one sampled texel and return plain trilinear. The clipmap's surface
// layers are uploaded WITHOUT mipmaps (ClipmapTerrain::setSurfaceLayer) and
// are sampled at level 0 by construction, which is what makes this exact here.
vec4 cmCubicTap(sampler2D tex, vec2 uv, vec2 size) {
    vec2 tc = uv * size - 0.5;
    vec2 f  = fract(tc);
    tc = floor(tc);

    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
    vec2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    vec2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    vec2 w3 = f3 / 6.0;

    // Two bilinear fetches per axis, each placed so the hardware's own lerp
    // delivers that pair of B-spline weights exactly. s0, s1 >= 1/6 always —
    // see the derivation above — so neither division needs a guard.
    vec2 s0 = w0 + w1;
    vec2 s1 = w2 + w3;
    vec2 t0 = (tc - 1.0 + w1 / s0 + 0.5) / size;
    vec2 t1 = (tc + 1.0 + w3 / s1 + 0.5) / size;

    // Clamped to the band of texel centres, the same band cmSurfLayer clamps
    // its single fetch to: the wider footprint may not read what the narrow
    // one could not.
    vec2 lo = 0.5 / size;
    vec2 hi = 1.0 - lo;
    t0 = clamp(t0, lo, hi);
    t1 = clamp(t1, lo, hi);

    return mix(mix(texture(tex, vec2(t0.x, t0.y)),
                   texture(tex, vec2(t1.x, t0.y)), s1.x),
               mix(texture(tex, vec2(t0.x, t1.y)),
                   texture(tex, vec2(t1.x, t1.y)), s1.x), s1.y);
}

// cmSurfLayer with the single bilinear fetch replaced by the four-tap cubic.
// The coverage weight is computed by the identical expression, so the two
// paths agree to the bit about where a layer stops — only what is read inside
// it differs.
vec4 cmSurfLayerCubic(sampler2D tex, vec3 a, vec2 sz, vec2 wxz, out float w) {
    if (sz.x < 0.5 || sz.y < 0.5) { w = 0.0; return vec4(0.0); }
    vec2 uv = ((wxz - a.xy) / a.z + 0.5) / sz;
    w = smoothstep(0.0, CM_FADE, cmEdge(uv, 0.0));
    return cmCubicTap(tex, uv, sz);
}

// cmSurface's chain, layer for layer and mix for mix, reading through the
// cubic filter. Kept structurally identical on purpose: the blend order, the
// coverage weights and the cmLayerFade factors are the ones the height chain
// agrees with, and a chain that reorganised them would fade the material away
// from the shape.
vec4 cmSurfaceCubic(vec2 wxz, float cDesired, out float present) {
    float n = u_surfaceCount;
    present = n > 0.5 ? 1.0 : 0.0;
    if (n < 0.5) return vec4(0.0);
    float w = 0.0;
    vec4 s = vec4(0.0);
    if      (n > 5.5) s = cmSurfLayerCubic(u_surface5, u_surf5A, u_surf5B, wxz, w);
    else if (n > 4.5) s = cmSurfLayerCubic(u_surface4, u_surf4A, u_surf4B, wxz, w);
    else if (n > 3.5) s = cmSurfLayerCubic(u_surface3, u_surf3A, u_surf3B, wxz, w);
    else if (n > 2.5) s = cmSurfLayerCubic(u_surface2, u_surf2A, u_surf2B, wxz, w);
    else if (n > 1.5) s = cmSurfLayerCubic(u_surface1, u_surf1A, u_surf1B, wxz, w);
    else              s = cmSurfLayerCubic(u_surface,  u_surfA,  u_surfB,  wxz, w);
    if (n > 5.5) { vec4 f = cmSurfLayerCubic(u_surface4, u_surf4A, u_surf4B, wxz, w); s = mix(s, f, w * cmLayerFade(u_surf4A.z, cDesired)); }
    if (n > 4.5) { vec4 f = cmSurfLayerCubic(u_surface3, u_surf3A, u_surf3B, wxz, w); s = mix(s, f, w * cmLayerFade(u_surf3A.z, cDesired)); }
    if (n > 3.5) { vec4 f = cmSurfLayerCubic(u_surface2, u_surf2A, u_surf2B, wxz, w); s = mix(s, f, w * cmLayerFade(u_surf2A.z, cDesired)); }
    if (n > 2.5) { vec4 f = cmSurfLayerCubic(u_surface1, u_surf1A, u_surf1B, wxz, w); s = mix(s, f, w * cmLayerFade(u_surf1A.z, cDesired)); }
    if (n > 1.5) { vec4 f = cmSurfLayerCubic(u_surface,  u_surfA,  u_surfB,  wxz, w); s = mix(s, f, w * cmLayerFade(u_surfA.z,  cDesired)); }
    return s;
}

// The switch. Object-like, so it renames the IDENTIFIER at every call site
// below without caring how many arguments the call carries; whole-identifier
// replacement means the neighbouring cmSurfaceUV / cmSurfaceUVAt / cmSurfaceCubic
// names an app's material chunk may define are untouched.
#define cmSurface cmSurfaceCubic
