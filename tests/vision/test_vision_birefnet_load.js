// Weights-gated: bro.vision.loadBirefnet takes the .safetensors file or a
// folder holding it (its model.safetensors, else its one .safetensors), as
// docs/vision-api.js says. A folder used to fail: the loader opened the path
// as the checkpoint file. Skips without bro.vision or without the weights
// (VISION_WEIGHTS_DIR, else ../brovisionml/weights; its birefnet/ folder).

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const WEIGHTS = process.env.VISION_WEIGHTS_DIR || '../brovisionml/weights';
const DIR = WEIGHTS + '/birefnet';
const FILE = DIR + '/model.safetensors';
const RENAMED = WEIGHTS + '/triposplat/background_removal';

const why = !bro.vision || bro.vision.available === false ? 'bro.vision is the stub'
    : !fs.existsSync(FILE) ? 'no BiRefNet weights at ' + FILE
    : '';

function run() {
    const W = 64, H = 64, data = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4, inside = (x - 32) * (x - 32) + (y - 32) * (y - 32) < 300;
        data[i] = inside ? 220 : 40; data[i + 1] = inside ? 60 : 160; data[i + 2] = inside ? 40 : 200; data[i + 3] = 255;
    }
    const img = { width: W, height: H, data };
    const matte = (src) => {
        const m = bro.vision.loadBirefnet(src, { modelSize: 256 });
        try { return m.removeBackground(img).alpha; } finally { m.dispose(); }
    };

    const fromFile = matte(FILE);
    assert(fromFile.length === W * H, 'the file loads and runs');
    let fromDir = null, err = null;
    try { fromDir = matte(DIR); } catch (e) { err = e; }
    assert(!err, 'a folder holding model.safetensors loads: ' + (err && err.message));
    let diff = 0;
    for (let i = 0; i < W * H; i++) diff = Math.max(diff, Math.abs(fromDir[i] - fromFile[i]));
    assert(diff < 1e-4, 'the folder loads the same weights as the file (max diff ' + diff + ')');

    // A folder whose one checkpoint has another name: bro.triposplat's
    // background_removal/birefnet.safetensors.
    if (fs.existsSync(RENAMED + '/birefnet.safetensors')) {
        err = null;
        try { matte(RENAMED); } catch (e) { err = e; }
        assert(!err, 'a folder with one .safetensors of another name loads: ' + (err && err.message));
    } else console.log('  (no ' + RENAMED + ': the renamed-checkpoint case skipped)');

    const tmp = path.join(os.tmpdir(), 'birefnet-load-' + process.pid + '-' + Date.now());
    fs.mkdirSync(tmp, { recursive: true });
    try {
        err = null;
        try { matte(tmp); } catch (e) { err = e; }
        assert(err && /no \.safetensors/.test(err.message), 'an empty folder throws, naming why: ' + (err && err.message));
    } finally {
        fs.rmSync(tmp, { recursive: true, force: true });
    }
}

if (why) skipTest(why); else run();
console.log('PASS');
