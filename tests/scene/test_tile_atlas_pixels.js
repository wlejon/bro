// createTileWorld({ atlasPixels }) accepts any byte view: the
// Uint8ClampedArray that getImageData().data returns used to reach the native
// as an index-keyed JSON object and the world rendered untextured.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('no scene; skipping tile atlas pixels test');
} else {
    scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scene.setAmbient({ intensity: 1.0 });

    const px = (x, y) => {
        const img = scene.captureFrame();
        const i = (y * img.width + x) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    };
    const s = (p) => p.r + ',' + p.g + ',' + p.b;
    const settle = () => { flush(); advanceTime(50); flush(); };

    // Straight down over a 4x4 grid: a cell is a 32 px block.
    const cam = scene.createCamera({ near: 0.1, far: 100 });
    cam.projection = 'orthographic';
    cam.size = 4;
    scene.setActiveCamera(cam);
    cam.position = [2, 20, 2.5];
    cam.lookAt(2, 0, 2);

    // A two-cell atlas, red | blue, big enough that filtering does not bleed.
    const W = 64, H = 32;
    const bytes = [];
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) bytes.push(...(x < W / 2 ? [255, 0, 0, 255] : [0, 0, 255, 255]));
    }

    function render(pixels, label) {
        const world = scene.createTileWorld({
            width: 4, height: 4, cellSize: 1.0, heightStep: 0.5, chunkSize: 4,
            atlasPixels: pixels, atlasWidth: W, atlasHeight: H,
            atlasColumns: 2, atlasRows: 1, tileAtlas: [0, 0, 1],
        });
        world.fillTile(0, 0, 1, 3, 1);   // left half: cell 0, red
        world.fillTile(2, 0, 3, 3, 2);   // right half: cell 1, blue
        world.rebuild();
        settle();
        const left = px(32, 64), right = px(96, 64);
        assert(left.r > 100 && left.b < 60, label + ': tile 1 draws the red cell: ' + s(left));
        assert(right.b > 100 && right.r < 60, label + ': tile 2 draws the blue cell: ' + s(right));
        world.destroy();
        settle();
    }

    render(new Uint8Array(bytes), 'Uint8Array');
    render(new Uint8ClampedArray(bytes), 'Uint8ClampedArray');
    const c2d = document.createElement('canvas').getContext('2d');
    const img = c2d.createImageData(W, H);
    img.data.set(bytes);
    render(img.data, 'ImageData.data');
    render(new Uint8Array(bytes).buffer, 'ArrayBuffer');

    cam.destroy();
    console.log('tile atlas pixels: ok');
}
