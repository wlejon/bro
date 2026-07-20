// Clipmap terrain — vertex chunk, spliced into mesh.vert at //__USER_CHUNK__.
//
// The mesh is a set of concentric square rings of FIXED topology, built once
// (see ClipmapTerrain::buildGeometry). Ring l has cell size c_l = c0 * 2^l and
// carries its level index l in aUV.x. Nothing about the geometry changes as
// the camera moves — this shader is what makes it follow.
//
// CRACK-FREE BY CONSTRUCTION. Displacement is a pure function of world XZ:
//
//     h = cmHeight(worldXZ, cmCellSize(worldXZ))
//       + cmDetail(worldXZ, cmCellSize(worldXZ))
//
// No term looks at the level index, so two rings meeting at a boundary
// evaluate the SAME function at the SAME position and land on the same height.
// There is nothing to stitch. cmCellSize is continuous in distance, so the mip
// level and the detail band limit are continuous too — GL's trilinear filter
// removes LOD popping and the octave fades cover the rest.
//
// Output is CAMERA-RELATIVE (mesh.vert's vWorldPos convention). ClipmapTerrain
// parks the node at the camera eye every update, so uModel is identity and the
// object-space position this hook writes IS the camera-relative world position.
// Keeping object space small matters — the whole matrix pipeline is fp32.

// WHY A MORPH BAND. Rings overlap on purpose (see kHoleInset in
// ClipmapTerrain::buildGeometry): per-level snapping offsets two neighbouring
// levels by up to one coarse cell, and insetting the hole is what stops that
// offset from opening a sliver of nothing.
//
// The overlap was assumed harmless because both rings evaluate the same pure
// function of world XZ. They do — AT THEIR VERTICES. Between vertices the
// rasteriser interpolates linearly, and the two rings have different vertex
// spacing (c_l against c_l/2), so across the overlap the coarse ring's flat
// quad and the fine ring's two quads describe DIFFERENT surfaces. They differ
// by the coarse ring's interpolation error, ~detailRelief * c_l / 2 — tens of
// metres at close levels, hundreds far out. The coarse sheet floats over the
// fine one and renders as a shingle lying across the mountainside, one band per
// level boundary.
//
// The fix is to make the claim true. Over the outermost cells of level l, each
// vertex morphs toward what level l+1 LINEARLY INTERPOLATES at that point, so
// by the time the overlap starts the two surfaces coincide everywhere, not just
// at shared vertices. Level l's grid is exactly twice as fine as level l+1's
// and both centres are multiples of 2*c_l, so a vertex sits on a level-l+1 grid
// point exactly when its index is even — the coarse interpolant is then the
// average of its neighbours one cell away, along whichever axes are odd. That
// parity is fixed for the life of the mesh, so it is baked into aUV.y.

// The full displaced surface at a world position. Both the vertex stage and
// the morph's neighbour taps go through this, so "what the coarser ring would
// interpolate" is built from the same function the coarser ring evaluates.
float cmSurface(vec2 wxz) {
    float c      = cmCellSize(wxz);
    vec2  rel    = wxz - u_camXZ;
    float floorM = cmDataFloor(wxz);
    // Same spacing rule as the fragment stage — see the long comment there.
    // Both stages have to agree on the slope, since it modulates the detail
    // that displaces here and shades there.
    float e  = max(c, floorM);
    float h0 = cmHeight(wxz, c);
    float hx = cmHeight(wxz + vec2(e, 0.0), c);
    float hz = cmHeight(wxz + vec2(0.0, e), c);
    float slope = cmSlopeFrom(h0, hx, hz, e);
    float amp;
    return h0 + cmDetailWeight(slope) * cmDetail(rel, c, floorM, amp).x
              + cmExemplarH(rel, c, floorM);
}

// Cells from the level's outer edge over which the morph runs. The overlap
// itself is at most kHoleInset coarse cells plus one snap step — 6 fine cells —
// so the morph must be COMPLETE before then; the rest is ramp, wide enough that
// the blend is not a visible kink.
const float CM_MORPH_FULL = 7.0;    // fully morphed within this many cells
const float CM_MORPH_RAMP = 9.0;    // ramping over this many more

// ---------------------------------------------------------------------------

void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
    float level = uv.x;                    // baked per ring

    // ZOOM. The ring offsets in the VBO are baked at the CONFIGURED cell size;
    // u_cellScale re-reads that same geometry at a coarser one, which is how
    // the stack's reach grows with altitude without a rebuild or a triangle.
    // u_cellSize is already the scaled c0, so cl is consistent with cmCellSize.
    pos.xz     *= u_cellScale;
    float cl    = u_cellSize * exp2(level);

    // Each level snaps its own centre to its own grid, so the terrain does not
    // swim under a moving camera — a vertex stays over the same world XZ until
    // the whole ring jumps a full cell. The 2*cl step keeps a level's grid
    // aligned with the next coarser level's grid as well.
    float snap   = 2.0 * cl;
    vec2  centre = floor(u_camXZ / snap) * snap;

    vec2  wxz = centre + pos.xz;

    // The surface this ring would draw on its own. cmSurface carries the
    // detail band and the exemplar as well as the layers — the exemplar
    // displaces rather than only shading, because terrain whose silhouette
    // stays flat while its shading says otherwise reads as a painted plane.
    float h = cmSurface(wxz);

    // MORPH toward the next coarser ring across the outer band. Chebyshev
    // distance in CELLS from this level's own centre, because the rings are
    // squares and the overlap is a fixed number of cells wide regardless of
    // level. u_invK is 4/N, so 2/u_invK is N/2 — the half-extent in cells.
    float g     = max(abs(pos.x), abs(pos.z)) / cl;
    float outer = 2.0 / u_invK;
    float a     = clamp((g - (outer - CM_MORPH_FULL - CM_MORPH_RAMP))
                        / CM_MORPH_RAMP, 0.0, 1.0);

    // aUV.y is the vertex's index parity: 0 both even (already a coarse grid
    // point, nothing to morph), 1 odd in x, 2 odd in z, 3 odd in both. The
    // coarsest level has no coarser neighbour and is baked 0 throughout.
    float parity = uv.y;
    if (a > 0.0 && parity > 0.5) {
        float hc;
        if (parity < 1.5) {
            hc = 0.5 * (cmSurface(wxz - vec2(cl, 0.0))
                      + cmSurface(wxz + vec2(cl, 0.0)));
        } else if (parity < 2.5) {
            hc = 0.5 * (cmSurface(wxz - vec2(0.0, cl))
                      + cmSurface(wxz + vec2(0.0, cl)));
        } else {
            // Cell centre: the bilinear value is the mean of the four corners.
            hc = 0.25 * (cmSurface(wxz + vec2(-cl, -cl))
                       + cmSurface(wxz + vec2( cl, -cl))
                       + cmSurface(wxz + vec2(-cl,  cl))
                       + cmSurface(wxz + vec2( cl,  cl)));
        }
        h = mix(h, hc, a);
    }

    vec2 rel = wxz - u_camXZ;
    pos    = cmCurve(rel, h);              // bend the chart onto the planet
    normal = vec3(0.0, 1.0, 0.0);          // real normal is per-pixel

    // Hand the FLAT chart offset to the fragment stage. It used to reconstruct
    // it from vWorldPos.xz, which worked only while the two were the same
    // thing; cmCurve foreshortens horizontally, so inverting it per pixel would
    // mean an asin and would still be wrong by the h/R term. rel is affine
    // within a triangle, so the interpolator carries it exactly. The level and
    // parity that arrived in uv have both been consumed by now.
    uv = rel;
}
