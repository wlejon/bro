// Test the TileWorld shade map — exercises setShade / fillShade /
// setShadeMap / getShade in src/scene/tile_world.cpp, the uShade* upload in
// scene_renderer_mesh.cpp / scene_renderer_instanced.cpp and the cellShade()
// lookup in shaders/mesh.frag. See docs/tile-api.js "setShade".

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping tile shade test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setAmbient({ intensity: 0.5 });
    scene.createLight({ type: 'directional', intensity: 1.0, direction: [0.2, -1, 0.3] });

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2], a: img.data[i + 3] };
    };
    const lum = (p) => (p.r + p.g + p.b) / 3;
    const settle = () => { flush(); advanceTime(50); flush(); };

    // An orthographic camera over the grid centre, sized to the grid, so a
    // cell is a 16-pixel block with x to the right and z downward. It sits a
    // hair toward +z: looking exactly along the up vector leaves lookAt no
    // roll to choose, and the frame's orientation would then depend on the
    // camera's history.
    const cam = scene.createCamera({ near: 0.1, far: 100 });
    cam.projection = 'orthographic';
    cam.size = 8;
    scene.setActiveCamera(cam);
    const lookDownAt = (x, z) => { cam.position = [x, 20, z + 0.5]; cam.lookAt(x, 0, z); };
    const cellPx = (cx, cy) => [cx * 16 + 8, cy * 16 + 8];

    // ---- square world, 8x8, cell 1 ----------------------------------------------
    const world = scene.createTileWorld({
        width: 8, height: 8, cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
        palette: new Float32Array([0, 0, 0, 0, 0.7, 0.7, 0.7, 1]),
    });
    world.fillTile(0, 0, 7, 7, 1);
    world.rebuild();
    lookDownAt(4, 4);
    settle();

    assert(world.getShade(3, 3) === 1, 'shade defaults to 1');
    assert(world.getShade(-1, 3) === 1 && world.getShade(3, 99) === 1, 'OOB getShade reads 1');

    const litBefore = px(24, 24);
    assert(litBefore.a > 0 && lum(litBefore) > 30, `ground lit before shading (lum=${lum(litBefore)})`);

    // The top rows of the grid are the top of the frame.
    world.fillShade(0, 0, 7, 3, 0);
    settle();
    assert(lum(px(...cellPx(4, 1))) <= 1 && lum(px(...cellPx(4, 6))) > 30,
           'rows z < 4 shade the top half of the frame');
    world.fillShade(0, 0, 7, 7, 1);

    // Left half black, right half untouched.
    world.fillShade(0, 0, 3, 7, 0);
    assert(world.getShade(1, 1) === 0, 'fillShade stores 0');
    assert(world.getShade(4, 1) === 1, 'fillShade leaves cells outside the rectangle');
    settle();
    const dark = px(...cellPx(1, 1)), lit = px(...cellPx(6, 1));
    assert(dark.a > 0 && lum(dark) <= 1, `shaded ground renders black under a light (lum=${lum(dark)})`);
    assert(lum(lit) > 30, `unshaded ground stays lit (lum=${lum(lit)})`);
    assert(Math.abs(lum(lit) - lum(litBefore)) < 4, 'shading one cell leaves another alone');

    // A half shade halves the lit result, since it multiplies after lighting.
    world.setShade(5, 5, 0.5);
    assert(Math.abs(world.getShade(5, 5) - 0.5) < 0.01, 'getShade round-trips the quantized value');
    settle();
    const half = px(...cellPx(5, 5));
    assert(Math.abs(lum(half) - lum(lit) * 0.5) < 6,
           `half shade is half the lit colour (lit=${lum(lit)} half=${lum(half)})`);

    // Clamping.
    world.setShade(6, 6, 7.0);
    world.setShade(6, 7, -3.0);
    assert(world.getShade(6, 6) === 1 && world.getShade(6, 7) === 0, 'setShade clamps to 0..1');

    // A placed object on a black cell is black too; on a lit cell it is lit.
    const kind = world.addObjectKind(Mesh.box(0.8, 0.6, 0.8), { color: [0.9, 0.9, 0.9, 1], castsShadow: false });
    assert(kind >= 0, 'addObjectKind succeeds');
    world.addObject(kind, 1, 5, { yOffset: 0.3 });
    world.addObject(kind, 6, 2, { yOffset: 0.3 });
    world.rebuildObjects();
    settle();
    const objDark = px(...cellPx(1, 5)), objLit = px(...cellPx(6, 2));
    assert(lum(objDark) <= 1, `object on a black cell is black (lum=${lum(objDark)})`);
    assert(lum(objLit) > lum(lit), `object on a lit cell stays lit (lum=${lum(objLit)} ground=${lum(lit)})`);

    // Bulk replace from a Float32Array, then a Uint8Array, then an Array.
    const f = new Float32Array(64).fill(1);
    f[7 * 8 + 7] = 0;
    world.setShadeMap(f);
    assert(world.getShade(1, 1) === 1 && world.getShade(7, 7) === 0, 'setShadeMap(Float32Array) replaces the map');
    settle();
    assert(lum(px(...cellPx(1, 1))) > 30, 'cleared cells render lit again');
    assert(lum(px(...cellPx(7, 7))) <= 1, 'the one cell left at 0 renders black');
    const u = new Uint8Array(64).fill(255);
    u[0] = 0;
    world.setShadeMap(u);
    assert(world.getShade(0, 0) === 0 && world.getShade(7, 7) === 1, 'setShadeMap(Uint8Array) takes 0..255');
    world.setShadeMap([1, 1, 0.25]);
    assert(world.getShade(0, 0) === 1 && Math.abs(world.getShade(2, 0) - 0.25) < 0.01 && world.getShade(3, 0) === 1,
           'setShadeMap(Array) fills what it covers');
    world.destroy();
    settle();

    // ---- hex world: the lookup follows the hex cell, not a square ---------------
    const hex = scene.createTileWorld({
        width: 6, height: 6, topology: 'hex', cellSize: 1.0, heightStep: 0.5, chunkSize: 6,
        palette: new Float32Array([0, 0, 0, 0, 0.7, 0.7, 0.7, 1]),
    });
    hex.fillTile(0, 0, 5, 5, 1);
    hex.rebuild();
    const c = hex.cellCenterWorldXZ(2, 2);
    const n = hex.cellCenterWorldXZ(3, 2);
    const d = hex.cellCenterWorldXZ(2, 3);
    lookDownAt(c.x, c.z);
    hex.setShade(2, 2, 0);
    settle();
    const toPx = (wx, wz) => [Math.round(64 + (wx - c.x) * 16), Math.round(64 + (wz - c.z) * 16)];
    const hc = px(...toPx(c.x, c.z));
    const hn = px(...toPx(n.x, n.z));
    const hd = px(...toPx(d.x, d.z));
    assert(lum(hc) <= 1, `hex cell centre is black (lum=${lum(hc)})`);
    assert(lum(hn) > 30, `the hex neighbour along the row stays lit (lum=${lum(hn)})`);
    assert(lum(hd) > 30, `the hex neighbour on the next row stays lit (lum=${lum(hd)})`);
    // Six points a third of the way to each corner sit inside the black cell.
    for (let i = 0; i < 6; i++) {
        const a = (30 - 60 * i) * Math.PI / 180;
        const p = px(...toPx(c.x + Math.cos(a) * 0.55, c.z + Math.sin(a) * 0.55));
        assert(lum(p) <= 1, `hex interior toward corner ${i} is black (lum=${lum(p)})`);
    }
    hex.destroy();
    settle();
}

document.body.removeChild(canvas);
