// Clipmap terrain — height source shared by the vertex and fragment chunks.
//
// GLSL 330 has no #include, so ClipmapTerrain::ensureNode() concatenates this
// file in front of BOTH chunks before handing them to setCustomShader. It used
// to be pasted into each file with a comment asking future editors to keep the
// copies byte-identical; a shared source that cannot drift is better than a
// promise that it will not. The height function MUST be identical across the
// two stages or the surface the fragment shades stops being the surface the
// vertex built.

uniform sampler2D u_h0;   // finest layer ... u_h3 coarsest
uniform sampler2D u_h1;
uniform sampler2D u_h2;
uniform sampler2D u_h3;

// Per layer: a = (originX, originZ, metresPerCell), b = (width, height) in
// texels. b.x < 0.5 marks the slot absent (its blend weight is forced to 0).
uniform vec3 u_l0a;  uniform vec2 u_l0b;
uniform vec3 u_l1a;  uniform vec2 u_l1b;
uniform vec3 u_l2a;  uniform vec2 u_l2b;
uniform vec3 u_l3a;  uniform vec2 u_l3b;

// Per layer, 1 = periodic in X. A global equirectangular chart has no east-west
// edge: column 0 continues column W-1, and the bake closes that join over a
// 1500 km band so the two sides are the same geography. The sampler is GL_REPEAT
// in S for such a layer, and its coverage ramp must not fade in X either — a
// fade there would reopen, as a hole, exactly the seam the bake closed.
uniform vec4 u_lWrapX;

// Per layer, 1 = the layer is BAND-LIMITED: it carries real content all the way
// down to twice its own cell size, so procedural detail may own everything finer
// and must own nothing coarser. 0 says nothing, and the cell-size heuristic in
// clipmap_detail.glsl (CM_ROUGHEN_*) then decides — a fine floor is read as a
// smooth learned window and roughened from a fixed ceiling instead.
//
// It is declared PER LAYER because it is a property of where the data came from,
// not of the clipmap: a stack may legitimately mix a smooth streamed window with
// a band-limited procedural one, and cell size cannot tell them apart.
uniform vec4 u_lBandLimited;

uniform vec2  u_camXZ;
uniform float u_camY;
uniform float u_cellSize;      // c0, level-0 cell size in metres (EFFECTIVE —
                               // already multiplied by u_cellScale)
uniform float u_cellScale;     // power-of-two zoom of the whole ring stack —
                               // see ClipmapTerrain::update
uniform float u_invK;          // 1/K = 4/N — see cmCellSize
uniform float u_pixelScale;    // 2*tan(fovY/2)/viewportHeight — see cmCellSize
uniform float u_layerCount;
uniform float u_heightScale;
uniform float u_seaLevel;
uniform float u_camGroundY;    // terrain height under the camera, world metres
uniform float u_planetRadius;  // metres; 0 = flat world, no curvature

// ---------------------------------------------------------------------------
// PLANETARY CURVATURE
//
// The height field stays a flat function of world XZ; this bends the SURFACE
// it describes onto a sphere. World XZ is read as arc length from the camera's
// ground point, i.e. an azimuthal-equidistant chart centred on the eye, so the
// cap is exact under the camera and stretches at its rim. That chart moves with
// the camera, which is fine for the near field and is what a cube-sphere
// replaces for the far field — but the cue it buys is the one that matters:
// the horizon lands where a planet of this radius puts it.
//
// Without this the world showed ground to 524 km from a 2 m eye height. Earth
// shows 5 km. That single number is why a correct-looking height field still
// read as a tabletop model: nothing else the renderer does can say "big" while
// the horizon says "small". It also BOUNDS work rather than adding it, because
// everything past the horizon now falls below the eye ray and is culled.
//
// Everything here is a pure function of (rel, h), so the crack-free guarantee
// survives: two rings meeting at a boundary bend identically.

// The camera-relative position of a point that sits `h` metres above the datum
// at flat offset `rel` from the camera.
vec3 cmCurve(vec2 rel, float h) {
    float R = u_planetRadius;
    if (R <= 0.0) return vec3(rel.x, h - u_camY, rel.y);

    float d  = length(rel);
    float th = d / R;                       // subtended angle

    // NOT (R+h)*cos(th) - R. That difference is ~d^2/(2R): at d = 1 km it is
    // 8 cm out of 6371 km, and fp32 carries seven digits, so the subtraction
    // returns zero and the whole near field stays flat. The half-angle form
    // computes the drop directly and is exact at every scale.
    float s  = sin(0.5 * th);
    float y  = h * cos(th) - 2.0 * R * s * s;

    // Horizontal foreshortening: arc length d maps to chord R*sin(th).
    // sin(th)/th is 0/0 at the camera, so series-expand under the threshold.
    float sinc = (th < 1e-4) ? 1.0 - th * th / 6.0 : sin(th) / th;

    vec2 xz = rel * ((1.0 + h / R) * sinc);
    return vec3(xz.x, y - u_camY, xz.y);
}

// Rotate a normal built in the flat chart into the curved frame. The local up
// tilts by exactly th, about the axis perpendicular to rel. Distant ground can
// subtend tens of degrees, so skipping this lights the far field as though it
// were still a plane and the terminator lands in the wrong place.
vec3 cmCurveNormal(vec2 rel, vec3 n) {
    float R = u_planetRadius;
    float d = length(rel);
    if (R <= 0.0 || d < 1e-3) return n;

    float th = d / R;
    vec2  u  = rel / d;
    float ct = cos(th), st = sin(th);

    // Flat basis: e1 along rel, e2 up, e3 across. Only e1 and e2 rotate.
    float a = dot(n, vec3(u.x, 0.0, u.y));   // along
    float b = n.y;                           // up
    float c = dot(n, vec3(-u.y, 0.0, u.x));  // across
    vec3  E1 = vec3( ct * u.x, -st, ct * u.y);
    vec3  E2 = vec3( st * u.x,  ct, st * u.y);
    vec3  E3 = vec3(-u.y, 0.0, u.x);
    return normalize(a * E1 + b * E2 + c * E3);
}

// A layer's outermost 8% (per axis) ramps its weight from 1 down to 0, so
// running off a fine layer's footprint degrades gradually into the coarser
// layer under it instead of showing a hard seam.
const float CM_FADE = 0.08;

// How many pixels one sampled cell should span. Below 1 the field is sampled
// finer than the framebuffer can show and aliases; well above 1 it visibly
// blurs. 1.5 keeps a margin over the Nyquist limit without softening detail
// the viewer can actually resolve.
const float CM_PIXELS_PER_CELL = 1.5;

// The same, for detail that is only ever SHADED rather than displaced. It is
// deliberately coarser. A height sampled near Nyquist looks slightly soft; a
// normal sampled near Nyquist sparkles, because shading responds to the
// derivative and so loses its footing about an octave earlier. Sharing one
// constant meant either grainy distance or blurred distance, with no setting
// that gave both.
const float CM_SHADE_PIXELS_PER_CELL = 3.5;

// Distance to the nearest edge this layer actually HAS, in uv. A periodic layer
// has only two, north and south, so X is left out of the minimum entirely
// rather than fed through a wrap — the point is that there is no edge there to
// be near.
float cmEdge(vec2 uv, float wrapX) {
    float v = min(uv.y, 1.0 - uv.y);
    return (wrapX > 0.5) ? v : min(v, min(uv.x, 1.0 - uv.x));
}

// One layer's height sample. `w` is its blend weight: 1 well inside the
// layer's extent, smoothly 0 at (and outside) its edge.
float cmLayer(sampler2D tex, vec3 a, vec2 sz, vec2 wxz, float cDesired,
              float wrapX, out float w) {
    if (sz.x < 0.5 || sz.y < 0.5) { w = 0.0; return 0.0; }
    vec2 t  = (wxz - a.xy) / a.z;      // position in texels; texel 0 at origin
    vec2 uv = (t + 0.5) / sz;          // -> texel centres
    // Fractional mip: the layer is sampled at whatever footprint this part of
    // the ring actually needs. Clamped at 0 — there is no level finer than 0.
    float lod = max(log2(cDesired / a.z), 0.0);
    // uv.x is deliberately left outside [0,1] when the layer wraps. GL_REPEAT
    // resolves it, and it does so ACROSS MIP LEVELS, which a fract() here could
    // not: a manual wrap leaves a texel-wide discontinuity that every coarser
    // level widens into a visible meridian.
    w = smoothstep(0.0, CM_FADE, cmEdge(uv, wrapX));
    return textureLod(tex, uv, lod).r;
}

// A layer's coverage weight alone, without the texture fetch — the same ramp
// cmLayer applies, so the two agree on where a layer stops.
float cmCoverage(vec3 a, vec2 sz, vec2 wxz, float wrapX) {
    if (sz.x < 0.5 || sz.y < 0.5) return 0.0;
    vec2 uv = ((wxz - a.xy) / a.z + 0.5) / sz;
    return smoothstep(0.0, CM_FADE, cmEdge(uv, wrapX));
}

// The finest cell size the DATA actually resolves here, in metres.
//
// This is the upper end of the band procedural detail has to fill. The pyramid
// is not uniform across the world: the finest layer is a window that follows
// the camera, so a point inside it is described down to 30 m while a point
// beyond it has nothing finer than the coarse field's 7.68 km. Without this,
// detail starts at one constant everywhere and the spectrum between the coarse
// cell and that constant is simply missing — kilometres of terrain that the
// pixel can resolve and nothing generates, which is what makes distant ground
// read as flat paint.
//
// Blended in LOG2 with the same weights cmHeight uses, so the floor crosses a
// layer edge exactly as smoothly as the height does — a geometric quantity
// deserves a geometric mean, and the octave selection downstream is log-scaled
// anyway.
//
// `bandLimited` rides along because it answers a question about the same blend:
// WHOSE floor is this. It is carried through the identical weights, so a stack
// that mixes a smooth streamed window with a band-limited procedural layer
// crosses between their two high-pass rules exactly as smoothly as the floor
// itself crosses — and a stack that declares nothing gets 0 everywhere, which is
// what leaves the heuristic in sole charge.
float cmDataFloor(vec2 wxz, out float bandLimited) {
    float n = u_layerCount;
    float w = 0.0;
    float f = 0.0;
    float b = 0.0;
    if      (n > 3.5) { f = log2(u_l3a.z); b = u_lBandLimited.w; }
    else if (n > 2.5) { f = log2(u_l2a.z); b = u_lBandLimited.z; }
    else if (n > 1.5) { f = log2(u_l1a.z); b = u_lBandLimited.y; }
    else if (n > 0.5) { f = log2(u_l0a.z); b = u_lBandLimited.x; }
    if (n > 3.5) { w = cmCoverage(u_l2a, u_l2b, wxz, u_lWrapX.z); f = mix(f, log2(u_l2a.z), w); b = mix(b, u_lBandLimited.z, w); }
    if (n > 2.5) { w = cmCoverage(u_l1a, u_l1b, wxz, u_lWrapX.y); f = mix(f, log2(u_l1a.z), w); b = mix(b, u_lBandLimited.y, w); }
    if (n > 1.5) { w = cmCoverage(u_l0a, u_l0b, wxz, u_lWrapX.x); f = mix(f, log2(u_l0a.z), w); b = mix(b, u_lBandLimited.x, w); }
    bandLimited = b;
    return exp2(f);
}

// Multi-scale height. Start from the COARSEST present layer (assumed to cover
// everything), then blend each finer layer in by its coverage weight.
// GL 3.3 cannot index a sampler array dynamically, so this is unrolled.
float cmHeight(vec2 wxz, float cDesired) {
    float n = u_layerCount;
    float w = 0.0;
    float h = 0.0;
    if      (n > 3.5) h = cmLayer(u_h3, u_l3a, u_l3b, wxz, cDesired, u_lWrapX.w, w);
    else if (n > 2.5) h = cmLayer(u_h2, u_l2a, u_l2b, wxz, cDesired, u_lWrapX.z, w);
    else if (n > 1.5) h = cmLayer(u_h1, u_l1a, u_l1b, wxz, cDesired, u_lWrapX.y, w);
    else if (n > 0.5) h = cmLayer(u_h0, u_l0a, u_l0b, wxz, cDesired, u_lWrapX.x, w);
    if (n > 3.5) { float s = cmLayer(u_h2, u_l2a, u_l2b, wxz, cDesired, u_lWrapX.z, w); h = mix(h, s, w); }
    if (n > 2.5) { float s = cmLayer(u_h1, u_l1a, u_l1b, wxz, cDesired, u_lWrapX.y, w); h = mix(h, s, w); }
    if (n > 1.5) { float s = cmLayer(u_h0, u_l0a, u_l0b, wxz, cDesired, u_lWrapX.x, w); h = mix(h, s, w); }
    return u_seaLevel + u_heightScale * h;
}

// --- surface (control-channel) layers ---------------------------------------
//
// Three channels per texel describing what the ground IS rather than where it
// is, on the same footing as the height stack: finest-first, contiguous from 0,
// blended by the identical coverage ramp so control data and height cross a
// layer edge together. They used to be a single layer, which made every channel
// a property of the finest chart and left the other 90% of a wide frame with
// either one clamped texel or a fade to neutral.
//
// The blend is LINEAR PER CHANNEL. That is correct for a quantity and wrong for
// an ID — see ClipmapTerrain::setSurfaceLayer for the argument. Do not put a
// biome index in here and expect it to survive a fade.
uniform sampler2D u_surface;    // finest ... u_surface3 coarsest
uniform sampler2D u_surface1;
uniform sampler2D u_surface2;
uniform sampler2D u_surface3;
uniform vec3  u_surfA;          // (originX, originZ, metresPerCell) per layer
uniform vec2  u_surfB;          // (width, height) in texels
uniform vec3  u_surf1A;
uniform vec2  u_surf1B;
uniform vec3  u_surf2A;
uniform vec2  u_surf2B;
uniform vec3  u_surf3A;
uniform vec2  u_surf3B;
uniform float u_surfaceCount;

// One surface layer's sample plus its coverage weight. Control channels are
// sampled at level 0 rather than through a fractional mip like cmLayer: a
// blurred hardness or moisture reads as a soft gradient, which is harmless,
// whereas the height stack's mip choice exists to stop the DISPLACEMENT
// aliasing and has no counterpart here.
vec3 cmSurfLayer(sampler2D tex, vec3 a, vec2 sz, vec2 wxz, out float w) {
    if (sz.x < 0.5 || sz.y < 0.5) { w = 0.0; return vec3(0.0); }
    vec2 uv = ((wxz - a.xy) / a.z + 0.5) / sz;
    w = smoothstep(0.0, CM_FADE, cmEdge(uv, 0.0));
    vec2 half_ = 0.5 / sz;
    return texture(tex, clamp(uv, half_, 1.0 - half_)).rgb;
}

// Multi-scale control channels. Same shape as cmHeight: start from the coarsest
// present layer, blend each finer one in by its coverage weight. Unrolled for
// the same reason — GL 3.3 cannot index a sampler array dynamically.
//
// `present` comes back 0 when the stack is empty, so a caller can keep its
// existing "no surface layer" branch rather than inventing a neutral value here
// — what neutral MEANS is the material's business, not the clipmap's.
vec3 cmSurface(vec2 wxz, out float present) {
    float n = u_surfaceCount;
    present = n > 0.5 ? 1.0 : 0.0;
    if (n < 0.5) return vec3(0.0);
    float w = 0.0;
    vec3 s = vec3(0.0);
    if      (n > 3.5) s = cmSurfLayer(u_surface3, u_surf3A, u_surf3B, wxz, w);
    else if (n > 2.5) s = cmSurfLayer(u_surface2, u_surf2A, u_surf2B, wxz, w);
    else if (n > 1.5) s = cmSurfLayer(u_surface1, u_surf1A, u_surf1B, wxz, w);
    else              s = cmSurfLayer(u_surface,  u_surfA,  u_surfB,  wxz, w);
    if (n > 3.5) { vec3 f = cmSurfLayer(u_surface2, u_surf2A, u_surf2B, wxz, w); s = mix(s, f, w); }
    if (n > 2.5) { vec3 f = cmSurfLayer(u_surface1, u_surf1A, u_surf1B, wxz, w); s = mix(s, f, w); }
    if (n > 1.5) { vec3 f = cmSurfLayer(u_surface,  u_surfA,  u_surfB,  wxz, w); s = mix(s, f, w); }
    return s;
}

// Desired cell size at a world position — a CONTINUOUS function of distance
// from the camera, never of the discrete level index. That continuity is what
// keeps the displacement single-valued across a ring boundary.
//
// K derivation: ring l spans a half-extent of (N/2) * c_l, and its central
// hole a half-extent of (N/4) * c_l (= the outer half-extent of ring l-1, so
// the rings tile exactly). Requiring the desired cell size at ring l's OUTER
// edge to equal the next ring's cell size:
//     dist / K = 2 * c_l   with   dist = (N/2) * c_l
//     => (N/2) * c_l / K = 2 * c_l  =>  K = N/4      (u_invK = 4/N)
// which also makes the desired size exactly c_l at ring l's INNER edge, so
// each ring is sampled between 1x and 2x its own cell size everywhere.
//
// Chebyshev distance, because the rings are squares — the level set of
// max(|dx|,|dz|) is a square that lines up with a ring edge.
//
// Climbing coarsens the whole field together (see the anti-aliasing term
// below), so detail always matches the altitude you are viewing from rather
// than being pinned to a ground-relative radius.
//
// All of this stays a pure function of world XZ within a frame — u_camXZ,
// u_camY and u_pixelScale are uniform across the draw — so the crack-free
// guarantee holds: two rings meeting at a boundary still evaluate the same
// height there.
// The anti-aliasing limit alone, with no geometry floor: the finest detail a
// PIXEL can resolve at this point, whether or not the mesh could express it.
//
// The fragment stage shades from an analytic normal, so it is not held to the
// ring's cell size the way the vertex stage is. Band-limiting fragment detail
// by the geometry cell was costing exactly the decades that make ground read as
// ground: with a one-metre finest ring, nothing below a metre could ever be
// shaded, no matter how close you stood. Splitting the two limits is what lets
// the surface keep gaining detail as you approach it, long after the triangles
// have run out.
float cmCellSizeAA(vec2 wxz) {
    vec2  d    = abs(wxz - u_camXZ);
    float dxz  = max(d.x, d.y);
    float dy   = abs(u_camY - u_camGroundY);
    float dist = max(dxz, dy);
    return dist * u_pixelScale * CM_SHADE_PIXELS_PER_CELL;
}

float cmCellSize(vec2 wxz) {
    vec2 d = abs(wxz - u_camXZ);
    float dxz = max(d.x, d.y);

    // Two independent limits, and the coarser one wins.
    //
    // GEOMETRY: a ring cannot express detail finer than its own cell, and ring
    // cell size grows with horizontal distance because that is how the rings
    // are laid out. Chebyshev distance, because the level sets of max() are
    // squares that line up with the ring edges — that is what makes K = N/4
    // exact.
    float cGeo = max(u_cellSize, dxz * u_invK);

    // ANTI-ALIASING: sampling detail finer than a pixel aliases. This limit is
    // about the eye, not the ground, so it uses true distance INCLUDING
    // altitude and is scaled by the projected size of a pixel. It is what
    // makes the ground directly beneath the camera behave: horizontally it is
    // at distance zero and would sample the finest mip, but from 90 km up that
    // point is 90 km away and one pixel covers ~100 m, which produced a moire
    // grid across the whole surface.
    //
    // The altitude that matters is height above the TERRAIN, not above sea
    // level. Measuring from sea level means standing on a 1200 m mountain
    // coarsens the ground at your feet as though you were 1200 m above it —
    // the surface under a climber goes smooth exactly where they can see it
    // best. u_camGroundY is the terrain height under the eye, so this is a
    // real eye-to-surface distance.
    //
    // These must NOT share a constant. u_invK comes from the ring layout;
    // u_pixelScale is 2*tan(fovY/2)/viewportHeight, the world size of one pixel
    // per unit distance. Reusing u_invK for altitude over-coarsened by ~24x at
    // 90 km and flattened the world to a featureless plane.
    float dy    = abs(u_camY - u_camGroundY);
    float dist  = max(dxz, dy);
    float cAA   = dist * u_pixelScale * CM_PIXELS_PER_CELL;

    return max(cGeo, cAA);
}

// Surface slope in [0,1] from three height samples one cell apart.
//
// FORWARD differences, not central, and shared by both stages deliberately.
// The vertex stage needs a slope to modulate detail amplitude, and every extra
// tap there is paid once per vertex across every ring; forward differences cost
// two taps instead of four. The fragment stage gets the same estimate for free
// out of the samples it already takes for its normal, so both stages agree on
// how much detail this ground wants. The shading normal itself is still built
// from central differences — this is only the modulator.
float cmSlopeFrom(float h0, float hx, float hz, float e) {
    vec3 n = normalize(vec3(h0 - hx, e, h0 - hz));
    return clamp(1.0 - n.y, 0.0, 1.0);
}
