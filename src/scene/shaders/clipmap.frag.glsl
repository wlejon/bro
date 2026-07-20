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
    vec2  rel = vWorldPos.xz;
    vec2  wxz = rel + u_camXZ;
    float c   = cmCellSize(wxz);

    // Central differences one desired-cell apart: the gradient is measured at
    // the same scale the height is filtered at, so distant terrain reads as
    // smooth rather than as aliasing noise.
    float e  = c;
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

    float floorM = cmDataFloor(wxz);
    float dAmp;
    vec3  d  = cmDetail(rel, cs, floorM, dAmp);
    float dw = cmDetailWeight(slope);
    grad += dw * d.yz;

    // Learned structure, at its own amplitude and unmodulated — see cmExemplar.
    vec3 ex = cmExemplar(rel, cs, floorM);
    grad += ex.yz;

    vec3 n = normalize(vec3(-grad.x, 1.0, -grad.y));
    normal = n;

    float wy     = h0 + dw * d.x + ex.x;

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

#ifdef CM_DEBUG_EXEMPLAR
    // Unlit visualisations, for bisecting shading artifacts. Define the macro
    // at the top of this file and rebuild. Scales are chosen to be readable,
    // not calibrated — check for saturation before concluding a field is
    // smooth, which is a mistake this block has already caused once.
    //   1 exemplar height, banded every 20 m   2 exemplar gradient
    //   3 exemplar mip level, banded            4 cavity
    //   5 shading normal                        6 albedo
    baseColor = vec3(0.0);
    if (CM_DEBUG_EXEMPLAR == 1)      emissive = vec3(fract(ex.x / 20.0));
    else if (CM_DEBUG_EXEMPLAR == 2) emissive = vec3(0.5 + 4.0 * ex.y,
                                                     0.5 + 4.0 * ex.z, 0.5);
    else if (CM_DEBUG_EXEMPLAR == 3) emissive = vec3(fract(cmExemplarLod(cs)));
    else if (CM_DEBUG_EXEMPLAR == 4) emissive = vec3(0.5 + 0.5 * cavity);
    else if (CM_DEBUG_EXEMPLAR == 5) emissive = 0.5 + 0.5 * n;
    else                             emissive = m.albedo;
#endif
}
