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

uniform vec2  u_camXZ;
uniform float u_camY;
uniform float u_cellSize;      // c0, level-0 cell size in metres
uniform float u_invK;          // 1/K = 4/N — see cmCellSize
uniform float u_pixelScale;    // 2*tan(fovY/2)/viewportHeight — see cmCellSize
uniform float u_layerCount;
uniform float u_heightScale;
uniform float u_seaLevel;

// A layer's outermost 8% (per axis) ramps its weight from 1 down to 0, so
// running off a fine layer's footprint degrades gradually into the coarser
// layer under it instead of showing a hard seam.
const float CM_FADE = 0.08;

// How many pixels one sampled cell should span. Below 1 the field is sampled
// finer than the framebuffer can show and aliases; well above 1 it visibly
// blurs. 1.5 keeps a margin over the Nyquist limit without softening detail
// the viewer can actually resolve.
const float CM_PIXELS_PER_CELL = 1.5;

// One layer's height sample. `w` is its blend weight: 1 well inside the
// layer's extent, smoothly 0 at (and outside) its edge.
float cmLayer(sampler2D tex, vec3 a, vec2 sz, vec2 wxz, float cDesired,
              out float w) {
    if (sz.x < 0.5 || sz.y < 0.5) { w = 0.0; return 0.0; }
    vec2 t  = (wxz - a.xy) / a.z;      // position in texels; texel 0 at origin
    vec2 uv = (t + 0.5) / sz;          // -> texel centres
    // Fractional mip: the layer is sampled at whatever footprint this part of
    // the ring actually needs. Clamped at 0 — there is no level finer than 0.
    float lod = max(log2(cDesired / a.z), 0.0);
    w = smoothstep(0.0, CM_FADE,
                   min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
    return textureLod(tex, uv, lod).r;
}

// Multi-scale height. Start from the COARSEST present layer (assumed to cover
// everything), then blend each finer layer in by its coverage weight.
// GL 3.3 cannot index a sampler array dynamically, so this is unrolled.
float cmHeight(vec2 wxz, float cDesired) {
    float n = u_layerCount;
    float w = 0.0;
    float h = 0.0;
    if      (n > 3.5) h = cmLayer(u_h3, u_l3a, u_l3b, wxz, cDesired, w);
    else if (n > 2.5) h = cmLayer(u_h2, u_l2a, u_l2b, wxz, cDesired, w);
    else if (n > 1.5) h = cmLayer(u_h1, u_l1a, u_l1b, wxz, cDesired, w);
    else if (n > 0.5) h = cmLayer(u_h0, u_l0a, u_l0b, wxz, cDesired, w);
    if (n > 3.5) { float s = cmLayer(u_h2, u_l2a, u_l2b, wxz, cDesired, w); h = mix(h, s, w); }
    if (n > 2.5) { float s = cmLayer(u_h1, u_l1a, u_l1b, wxz, cDesired, w); h = mix(h, s, w); }
    if (n > 1.5) { float s = cmLayer(u_h0, u_l0a, u_l0b, wxz, cDesired, w); h = mix(h, s, w); }
    return u_seaLevel + u_heightScale * h;
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
    // These must NOT share a constant. u_invK comes from the ring layout;
    // u_pixelScale is 2*tan(fovY/2)/viewportHeight, the world size of one pixel
    // per unit distance. Reusing u_invK for altitude over-coarsened by ~24x at
    // 90 km and flattened the world to a featureless plane.
    float dy    = abs(u_camY - u_seaLevel);
    float dist  = max(dxz, dy);
    float cAA   = dist * u_pixelScale * CM_PIXELS_PER_CELL;

    return max(cGeo, cAA);
}
