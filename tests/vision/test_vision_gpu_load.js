// Weights-gated: every bro.vision loader loads on the default (GPU) device.
//
// The loaders used to call to(device) before load(), which a module refuses
// on CUDA ("dinov2::Backbone: to() called before load()"), so everything
// but loadBirefnet worked only with { device: 'cpu' }. Skips (passes with a
// message) without a GPU backend or without the checkpoints
// (VISION_WEIGHTS_DIR, else ../brovisionml/weights).

const fs = require('node:fs');

const DIR = process.env.VISION_WEIGHTS_DIR || '../brovisionml/weights';

if (!bro.vision || bro.vision.available === false) {
    console.log('bro.vision is the stub; skipping');
} else if (!bro.gpu || !bro.gpu.available) {
    console.log('no GPU backend; skipping');
} else if (!fs.existsSync(DIR + '/Depth-Anything-V2-Small/model.safetensors')) {
    console.log('no vision weights at ' + DIR + '; skipping');
} else {
    const loaders = [
        ['loadDepth', 'Depth-Anything-V2-Small'],
        ['loadNormal', 'dsine'],
        ['loadHed', 'hed'],
        ['loadLineart', 'lineart'],
        ['loadMlsd', 'mlsd'],
        ['loadOpenpose', 'openpose'],
        ['loadSegformer', 'segformer-b0-ade'],
        ['loadSam', 'sam-vit-base'],
    ];
    for (const [fn, sub] of loaders) {
        const dir = DIR + '/' + sub;
        if (!fs.existsSync(dir)) { console.log('  (' + fn + ': no ' + sub + ', skipped)'); continue; }
        let m = null, err = null;
        try { m = bro.vision[fn](dir); } catch (e) { err = e; }
        assert(!err, fn + ' loads on the default device: ' + (err && err.message));
        assert(m && m.device !== 'CPU', fn + ' is on the GPU (' + (m && m.device) + ')');
        if (m && m.dispose) m.dispose();
    }

    // And the loaded model runs there: a small gradient through depth.
    const W = 64, H = 64, data = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4;
        data[i] = x * 4; data[i + 1] = y * 4; data[i + 2] = 128; data[i + 3] = 255;
    }
    const depth = bro.vision.loadDepth(DIR + '/Depth-Anything-V2-Small');
    const r = depth.estimate({ width: W, height: H, data });
    assert(r && r.width === W && r.height === H && r.depth.length === W * H,
           'depth estimate on the GPU returns a full map');
    let finite = true;
    for (let i = 0; i < r.depth.length; i++) if (!Number.isFinite(r.depth[i])) { finite = false; break; }
    assert(finite, 'every depth value is finite');
}
console.log('PASS');
