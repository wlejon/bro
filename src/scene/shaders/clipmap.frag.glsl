// Clipmap terrain — fragment chunk, spliced into mesh.frag at //__USER_CHUNK__.
//
// Normals are reconstructed PER PIXEL from central differences of the same
// height function the vertex stage displaced with, at the same fractional mip
// level. Interpolated vertex normals would be as coarse as the ring cell the
// fragment sits in — metres wide out at the horizon rings.
//
// Shading is deliberately plain: a neutral base colour with slope-driven
// variation. Material work and procedural detail are a separate concern; this
// chunk exists to make the geometry readable.
//
// vWorldPos is camera-relative (mesh.frag's convention), so absolute world XZ
// is reconstructed as vWorldPos.xz + u_camXZ.

// ---------------------------------------------------------------------------

void userFragment(inout vec3 baseColor, inout vec3 normal, inout float metallic,
                  inout float roughness, inout vec3 emissive, inout float alpha) {
    vec2  wxz = vWorldPos.xz + u_camXZ;
    float c   = cmCellSize(wxz);

    // Central differences one desired-cell apart: the gradient is measured at
    // the same scale the height is filtered at, so distant terrain reads as
    // smooth rather than as aliasing noise.
    float e  = c;
    float hL = cmHeight(wxz - vec2(e, 0.0), c);
    float hR = cmHeight(wxz + vec2(e, 0.0), c);
    float hD = cmHeight(wxz - vec2(0.0, e), c);
    float hU = cmHeight(wxz + vec2(0.0, e), c);
    vec3  n  = normalize(vec3(hL - hR, 2.0 * e, hD - hU));

    normal = n;

    float slope = clamp(1.0 - n.y, 0.0, 1.0);
    baseColor = mix(vec3(0.40, 0.44, 0.36),    // flat ground
                    vec3(0.34, 0.31, 0.28),    // exposed slope
                    smoothstep(0.02, 0.35, slope));
    metallic  = 0.0;
    roughness = 0.95;
}
