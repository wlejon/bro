// TileWorld object kinds and placements can be removed and replaced
// (docs/tile-api.js): removeObjectKind drops a kind's draw at once and retires
// its id, replaceObjectKind swaps its mesh and material under the same
// placements, removeObject takes one placement out and shifts the rest down,
// and replaceObject moves one in place. Checked by pixel, straight down on an
// orthographic camera, and by the bookkeeping calls.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping tile object replace test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setAmbient({ intensity: 1.0 });

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    };
    const settle = () => { flush(); advanceTime(50); flush(); };
    const s = (p) => p.r + ',' + p.g + ',' + p.b;
    const dark = (p) => p.r < 40 && p.g < 40 && p.b < 40;

    const cam = scene.createCamera({ near: 0.1, far: 100 });
    cam.projection = 'orthographic';
    cam.size = 4;
    scene.setActiveCamera(cam);
    cam.position = [2, 20, 2.5];
    cam.lookAt(2, 0, 2);
    const cellPx = (cx, cy) => [cx * 32 + 16, cy * 32 + 16];
    const at = (cx, cy) => px(...cellPx(cx, cy));

    const world = scene.createTileWorld({
        width: 4, height: 4, cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
        palette: new Float32Array([0, 0, 0, 0, 0.05, 0.05, 0.05, 1]),
    });
    world.fillTile(0, 0, 3, 3, 1);
    world.rebuild();

    const style = (color) => ({ color, castsShadow: false });
    const block = () => Mesh.box(0.4, 0.2, 0.4);
    const red = world.addObjectKind(block(), style([1, 0, 0, 1]));
    const green = world.addObjectKind(block(), style([0, 1, 0, 1]));
    assert(red >= 0 && green >= 0 && red !== green, 'two kinds: ' + red + ', ' + green);
    world.addObject(red, 0, 0, { yOffset: 0.2 });
    world.addObject(red, 1, 0, { yOffset: 0.2 });
    world.addObject(green, 0, 2, { yOffset: 0.2 });
    world.rebuildObjects();
    settle();
    let p = at(0, 0);
    assert(p.r > 100 && p.g < 40, 'red kind draws red: ' + s(p));
    p = at(0, 2);
    assert(p.g > 100 && p.r < 40, 'green kind draws green: ' + s(p));

    // ── replaceObjectKind: a new mesh and material, the same placements ─────
    assert(world.replaceObjectKind(red, block(), style([0, 0, 1, 1])),
           'replaceObjectKind on a live kind succeeds');
    settle();
    p = at(0, 0);
    assert(p.b > 100 && p.r < 40, 'the replaced material draws at the old placement: ' + s(p));
    p = at(1, 0);
    assert(p.b > 100 && p.r < 40, 'and at every placement: ' + s(p));
    assert(world.objectCount(red) === 2, 'placements survive the replace: ' + world.objectCount(red));

    // A small mesh off to the side of the sampled pixel: the old geometry is gone.
    const aside = Mesh.box(0.05, 0.4, 0.05);
    aside.translate(0.35, 0, 0.35);
    assert(world.replaceObjectKind(red, aside), 'mesh-only replace keeps the material');
    settle();
    p = at(0, 0);
    assert(dark(p), 'the old mesh no longer draws after its replacement: ' + s(p));
    assert(world.replaceObjectKind(red, block()), 'restore the full-size mesh');
    settle();
    p = at(0, 0);
    assert(p.b > 100, 'a mesh-only replace keeps the replaced colour: ' + s(p));

    // ── removeObject / replaceObject ─────────────────────────────────────────
    assert(world.removeObject(red, 0), 'removeObject(kind, 0)');
    assert(!world.removeObject(red, 5), 'removeObject out of range is false');
    world.rebuildObjects();
    settle();
    assert(world.objectCount(red) === 1, 'one placement left');
    assert(dark(at(0, 0)), 'the removed placement is gone: ' + s(at(0, 0)));
    assert(at(1, 0).b > 100, 'the other placement stays: ' + s(at(1, 0)));

    assert(world.replaceObject(red, 0, 3, 3, { yOffset: 0.2 }), 'replaceObject moves index 0 (the old index 1)');
    assert(!world.replaceObject(red, 0, 9, 9), 'replaceObject onto a bad cell is false');
    world.rebuildObjects();
    settle();
    assert(dark(at(1, 0)), 'the moved placement left its old cell: ' + s(at(1, 0)));
    assert(at(3, 3).b > 100, 'and draws on its new one: ' + s(at(3, 3)));

    // ── removeObjectKind: the draw goes at once and the id is retired ───────
    assert(world.hasObjectKind(red), 'hasObjectKind before removal');
    assert(world.removeObjectKind(red), 'removeObjectKind on a live kind');
    settle();
    assert(dark(at(3, 3)), 'a removed kind no longer draws: ' + s(at(3, 3)));
    assert(!world.hasObjectKind(red), 'hasObjectKind is false after removal');
    assert(!world.removeObjectKind(red), 'a second removal is false');
    assert(world.objectCount(red) === 0, 'a removed kind has no placements');
    assert(world.addObject(red, 1, 1) === -1, 'addObject on a removed kind is -1');
    assert(!world.replaceObjectKind(red, Mesh.box(1, 1, 1)), 'replaceObjectKind on a removed kind is false');
    world.clearObjects(red);
    world.rebuildObjects();

    p = at(0, 2);
    assert(p.g > 100 && p.r < 40, 'the other kind is untouched: ' + s(p));
    const fresh = world.addObjectKind(block(), style([1, 1, 1, 1]));
    assert(fresh !== red && fresh > green, 'a new kind takes a fresh id: ' + fresh);
    world.addObject(fresh, 2, 1, { yOffset: 0.2 });
    world.addObject(green, 3, 1, { yOffset: 0.2 });
    world.rebuildObjects();
    settle();
    p = at(2, 1);
    assert(p.r > 100 && p.g > 100 && p.b > 100, 'the fresh kind draws: ' + s(p));
    p = at(3, 1);
    assert(p.g > 100 && p.r < 40, 'a kind added before the removal still takes placements: ' + s(p));

    // load() keeps live kinds and skips removed ones.
    const bytes = world.save();
    assert(world.load(bytes), 'load round-trips with a removed kind present');
    world.addObject(green, 0, 0, { yOffset: 0.2 });
    world.rebuildObjects();
    settle();
    p = at(0, 0);
    assert(p.g > 100 && p.r < 40, 'a live kind still draws after load(): ' + s(p));
    assert(!world.hasObjectKind(red), 'a removed kind stays removed across load()');

    world.destroy();
    cam.destroy();
    console.log('tile object replace: ok');
}
