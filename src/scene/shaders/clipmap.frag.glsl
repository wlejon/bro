// Clipmap terrain — fragment chunk, spliced into mesh.frag at //__USER_CHUNK__.
//
// Normals are reconstructed PER PIXEL from the same height function the vertex
// stage displaced with, at the same fractional mip level and the same detail
// band limit. Interpolated vertex normals would be as coarse as the ring cell
// the fragment sits in — metres wide out at the horizon rings.
//
// The detail's contribution to the normal is ANALYTIC: cmDetail carries its own
// derivative, so metre-scale relief shades correctly without a single extra
// texture tap, at any distance, including on ground the geometry is too coarse
// to have displaced. That is what lets detail read as surface rather than as
// silhouette.
//
// vWorldPos is camera-relative (mesh.frag's convention), so absolute world XZ
// is reconstructed as vWorldPos.xz + u_camXZ.

// ---------------------------------------------------------------------------

void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic,
                  inout float roughness, inout vec3 emissive, inout float alpha) {
    // The FLAT chart offset, forwarded through vUV by the vertex stage. Not
    // vWorldPos.xz — that is the curved position, and the height field is
    // indexed in the flat chart.
    vec2  rel = vUV;
    vec2  wxz = rel + u_camXZ;
    float c   = cmCellSize(wxz);

    // NEVER DIFFERENCE THE LAYER BELOW ITS OWN CELL.
    //
    // The obvious spacing is the rendered cell c, and it is wrong wherever the
    // data is coarser than the pixel — which is everywhere the coarse field is
    // the only layer. c is tens of metres; the coarse cell is 7.68 km. That
    // asks the hardware to resolve a height difference across a THOUSANDTH of a
    // texel, and bilinear filtering carries about eight bits of sub-texel
    // weight. So the interpolated height is a staircase with steps of
    // (texel height range)/256 — a couple of metres — and a difference taken
    // over 50 m turns a 2 m step into a 2-degree slope step held flat across
    // the whole quantisation cell.
    //
    // That is the shingling: hard-edged plates in the shading normal, aligned
    // to the layer's texture axes, growing with distance because the difference
    // baseline does, and present only when a height layer is installed. It was
    // never the exemplar, the mesh, shadows or the atmosphere, all of which
    // were bisected away while the layer itself was never suspected — the
    // height field looks perfectly smooth, because the defect is metres in a
    // field with kilometres of range and only its DERIVATIVE is ruinous.
    //
    // cmDataFloor is exactly the right spacing: the finest cell the pyramid
    // resolves here, blended across layer edges the same way the height is. The
    // layer has no slope information below it, so measuring there samples
    // filter noise instead of terrain. Detail and the exemplar are unaffected —
    // they carry analytic derivatives and fill the band under this floor.
    float floorM = cmDataFloor(wxz);
    float e  = max(c, floorM);
    float h0 = cmHeight(wxz, c);
    float hL = cmHeight(wxz - vec2(e, 0.0), c);
    float hR = cmHeight(wxz + vec2(e, 0.0), c);
    float hD = cmHeight(wxz - vec2(0.0, e), c);
    float hU = cmHeight(wxz + vec2(0.0, e), c);

    vec2 grad = vec2(hR - hL, hU - hD) / (2.0 * e);

    // hR and hU are exactly the taps the vertex stage took, so this is its
    // modulator, not an approximation of it.
    float slope = cmSlopeFrom(h0, hR, hU, e);

    // Detail and materials are limited by the PIXEL, not by the ring cell that
    // limits the geometry — see cmCellSizeAA. The mesh stops at c; shading
    // carries on past it, which is the whole reason ground keeps gaining detail
    // as you walk towards it.
    float cs = cmCellSizeAA(wxz);

    float dAmp;
    vec3  d  = cmDetail(rel, cs, floorM, dAmp);
    float dw = cmDetailWeight(slope);
    grad += dw * d.yz;

    // The gradient was taken in the flat chart, so this normal is relative to
    // the chart's up. On a planet the local up leans away from the eye with
    // distance; cmCurveNormal applies that lean. Material selection below still
    // wants the CHART normal — "how steep is this ground" is a property of the
    // terrain, not of where the camera happens to be standing.
    vec3 n = normalize(vec3(-grad.x, 1.0, -grad.y));
    normal = cmCurveNormal(rel, n);

    float wy     = h0 + dw * d.x;

    // Cavity is "how far below the local detail mean this point sits", so the
    // two sides of the ratio have to be the SAME quantity. dAmp is the summed
    // amplitude of the live octaves — the bound on |d.x| — and d.x is what that
    // sum actually came to. The slope weight dw scales both when the height is
    // built (see wy above), so carrying it on the denominator alone inflated
    // the ratio by up to 1/CM_DETAIL_FLOOR, i.e. 8x on flat ground.
    //
    // The floor mattered more. Where every octave has faded under the pixel
    // limit, d.x and dAmp go to zero TOGETHER and the ratio is 0/0; pinning the
    // denominator at a constant turned that into sign(d.x) — a hard binary mask
    // that multiplied albedo by 0.88 or 1.0 with nothing in between. Wherever
    // the exemplar was present the noise band starts at u_detailWavelength
    // instead of eight octaves above it, so that band died early and the mask
    // covered whole mountainsides: the plates that read as overlapping
    // translucent sheets. Fading the term out with the amplitude that feeds it
    // leaves distant ground with NO cavity, which is the honest answer, rather
    // than with a two-tone one.
    float conf   = smoothstep(0.0, 0.25, dAmp);
    float cavity = conf * clamp(d.x / max(dAmp, 1e-4), -1.0, 1.0);

    CmMaterial m;
    cmMaterialAt(rel, wy, n, cs, cavity, m);

    baseColor = m.albedo;
    metallic  = 0.0;
    roughness = m.roughness;
}
