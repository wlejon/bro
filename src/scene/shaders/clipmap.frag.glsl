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

    float dAmp;
    vec3  d  = cmDetail(rel, cs, cmDataFloor(wxz), dAmp);
    float dw = cmDetailWeight(slope);
    grad += dw * d.yz;

    vec3 n = normalize(vec3(-grad.x, 1.0, -grad.y));
    normal = n;

    float wy     = h0 + dw * d.x;
    // Normalised against the amplitude actually summed — the band's top moves
    // with the data floor, so u_detailWavelength no longer bounds d.x and
    // dividing by it would drive cavity to +-1 across all distant ground.
    float cavity = clamp(d.x / max(dw * dAmp, 1e-3), -1.0, 1.0);

    CmMaterial m;
    cmMaterialAt(rel, wy, n, cs, cavity, m);

    baseColor = m.albedo;
    metallic  = 0.0;
    roughness = m.roughness;
}
