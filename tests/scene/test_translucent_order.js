// Translucent meshes must blend back-to-front regardless of tree order —
// exercises the sorted translucent pass in src/scene/scene_renderer.cpp
// (render3D collects lit meshes / instanced nodes with uniform alpha < 1
// during the opaque walks and draws them depth-sorted after all opaque
// passes). Two overlapping translucent surfaces at different depths: the
// NEARER one must be drawn last so it dominates the "over" blend, from
// EITHER camera side. Before the sort, draws ran in tree order, so one of
// the two camera sides always composed wrong.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping translucent order test');
} else {
    // Linear tonemap so channel dominance survives to the LDR readback.
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

    const centerPixel = () => {
        const img = scene.captureFrame();
        const i = (64 * img.width + 64) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2],
                 a: img.data[i + 3] };
    };
    const lookFrom = (z) => scene.setCamera({
        fov: 60, near: 0.1, far: 100,
        position: [0, 0, z], target: [0, 0, 0], up: [0, 1, 0],
    });

    // =====================================================================
    // Case 1: two translucent MESHES. Red box at z=0 created FIRST, blue
    // box at z=2 created second. Tree order (red, blue) happens to be
    // back-to-front from the +z side; from the -z side it is front-to-back
    // — the regression the sorted pass fixes.
    // emissive:1 (emissiveColor defaults to baseColor) makes each surface's
    // hue independent of the light direction, so the same assertions hold
    // from both sides.
    // =====================================================================
    const red = scene.createMesh({
        mesh: 'box', color: [1, 0, 0, 0.5], emissive: 1, z: 0,
    });
    const blue = scene.createMesh({
        mesh: 'box', color: [0, 0, 1, 0.5], emissive: 1, z: 2,
    });

    // From +z: blue (z=2) is nearer -> drawn last -> blue dominates.
    lookFrom(8);
    let px = centerPixel();
    assert(px.a > 0, 'boxes cover the center pixel (+z side)');
    assert(px.b > px.r,
        `+z side: nearer blue dominates the blend (r=${px.r} b=${px.b})`);

    // From -z: red (z=0) is nearer -> must be drawn last -> red dominates.
    // Tree order would draw blue last here and get this wrong.
    lookFrom(-8);
    px = centerPixel();
    assert(px.a > 0, 'boxes cover the center pixel (-z side)');
    assert(px.r > px.b,
        `-z side: nearer red dominates the blend (r=${px.r} b=${px.b})`);

    // Sanity: the farther surface still contributes through the near one
    // (depth writes stay off for translucents — it must not be occluded).
    assert(px.b > 0,
        `-z side: farther blue still visible through red (b=${px.b})`);

    red.destroy();
    blue.destroy();

    // =====================================================================
    // Case 2: translucent MESH vs translucent INSTANCED node. Before the
    // sorted pass, the instanced pass always ran after the mesh pass, so a
    // FARTHER translucent instanced node was blended on top of a nearer
    // translucent mesh. Whole-node depth is the documented approximation
    // for instanced nodes.
    // =====================================================================
    lookFrom(8);
    // Nearer red mesh at z=2, farther blue instanced box at z=0.
    const nearMesh = scene.createMesh({
        mesh: 'box', color: [1, 0, 0, 0.5], emissive: 1, z: 2,
    });
    // Instance row: 3x4 affine (identity rotation, origin) + RGBA tint
    // (white; alpha is the atlas cell index).
    const inst = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                   1, 1, 1, 0]);
    const farInst = scene.createInstancedMesh({
        mesh: Mesh.box(), color: [0, 0, 1, 0.5], emissive: 1,
        instances: inst,
    });

    px = centerPixel();
    assert(px.a > 0, 'mesh + instanced cover the center pixel');
    assert(px.r > px.b,
        `nearer red mesh beats farther blue instanced node (r=${px.r} b=${px.b})`);
    assert(px.b > 0,
        `farther blue instanced node still visible through the mesh (b=${px.b})`);

    nearMesh.destroy();
    farInst.destroy();
    flush();
}

document.body.removeChild(canvas);
