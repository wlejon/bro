// TileWorld.addObject({ color }) tints that one instance (docs/tile-api.js
// addObject). The bronze wrapper dropped the key, so every instance drew in
// its kind's base colour.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping tile object tint test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setAmbient({ intensity: 1.0 });

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    };
    const settle = () => { flush(); advanceTime(50); flush(); };

    // Orthographic, straight down over a 4x4 grid: a cell is a 32 px block.
    const cam = scene.createCamera({ near: 0.1, far: 100 });
    cam.projection = 'orthographic';
    cam.size = 4;
    scene.setActiveCamera(cam);
    cam.position = [2, 20, 2.5];
    cam.lookAt(2, 0, 2);
    const cellPx = (cx, cy) => [cx * 32 + 16, cy * 32 + 16];

    const world = scene.createTileWorld({
        width: 4, height: 4, cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
        palette: new Float32Array([0, 0, 0, 0, 0.05, 0.05, 0.05, 1]),
    });
    world.fillTile(0, 0, 3, 3, 1);
    world.rebuild();

    const kind = world.addObjectKind(Mesh.box(0.8, 0.4, 0.8), { color: [1, 1, 1, 1], castsShadow: false });
    assert(kind >= 0, 'addObjectKind succeeds');
    world.addObject(kind, 0, 1, { yOffset: 0.2, color: [1, 0, 0, 1] });
    world.addObject(kind, 1, 1, { yOffset: 0.2, color: [0, 1, 0, 1] });
    world.addObject(kind, 2, 1, { yOffset: 0.2, color: [0, 0, 1] });   // RGB only: alpha stays 1
    world.addObject(kind, 3, 1, { yOffset: 0.2 });                        // untinted
    world.rebuildObjects();
    settle();

    const red = px(...cellPx(0, 1)), green = px(...cellPx(1, 1)), blue = px(...cellPx(2, 1)), white = px(...cellPx(3, 1));
    const s = (p) => p.r + ',' + p.g + ',' + p.b;
    assert(red.r > 100 && red.g < 40 && red.b < 40, 'red instance renders red: ' + s(red));
    assert(green.g > 100 && green.r < 40 && green.b < 40, 'green instance renders green: ' + s(green));
    assert(blue.b > 100 && blue.r < 40 && blue.g < 40, 'blue instance renders blue: ' + s(blue));
    assert(white.r > 100 && white.g > 100 && white.b > 100, 'an instance without color keeps the kind colour: ' + s(white));

    world.destroy();
    cam.destroy();
    console.log('tile object tint: ok');
}
