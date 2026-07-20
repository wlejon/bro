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
//
// Neither term looks at the level index, so two rings meeting at a boundary
// evaluate the SAME function at the SAME position and land on the same height.
// There is nothing to stitch. cmCellSize is continuous in distance, so the mip
// level is continuous too and GL's trilinear filter removes LOD popping.
//
// Output is CAMERA-RELATIVE (mesh.vert's vWorldPos convention). ClipmapTerrain
// parks the node at the camera eye every update, so uModel is identity and the
// object-space position this hook writes IS the camera-relative world position.
// Keeping object space small matters — the whole matrix pipeline is fp32.

// ---------------------------------------------------------------------------

void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv) {
    float level = uv.x;                    // baked per ring
    float cl    = u_cellSize * exp2(level);

    // Each level snaps its own centre to its own grid, so the terrain does not
    // swim under a moving camera — a vertex stays over the same world XZ until
    // the whole ring jumps a full cell. The 2*cl step keeps a level's grid
    // aligned with the next coarser level's grid as well.
    float snap   = 2.0 * cl;
    vec2  centre = floor(u_camXZ / snap) * snap;

    vec2  wxz = centre + pos.xz;
    float c   = cmCellSize(wxz);
    float h   = cmHeight(wxz, c);

    pos    = vec3(wxz.x - u_camXZ.x, h - u_camY, wxz.y - u_camXZ.y);
    normal = vec3(0.0, 1.0, 0.0);          // real normal is per-pixel
}
