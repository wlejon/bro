// Smoke test for bro.vision.loadBirefnet — standalone BiRefNet background
// removal (the same Swin-L checkpoint bro.triposplat consumes as its matting
// front-end, exposed as a first-class bro.vision model).
// Run against the minimal smoke app:
//   bro-headless tests/_smoke_app tests/_birefnet_smoke.js
// Needs the real checkpoint and the triposplat sample portrait.

function pumpUntil(pred, budgetMs) {
    const start = Date.now();
    while (!pred() && (Date.now() - start) < budgetMs) { sleep(20); }
    return pred();
}

const fs = require('fs');
const WEIGHTS = 'D:/projects/brovisionml/weights/triposplat/background_removal/birefnet.safetensors';
const SAMPLE  = '../broworkshop/demos/triposplat/samples/portrait.png';
assert(fs.existsSync(WEIGHTS), 'checkpoint exists');

// ── load (async convention) ──────────────────────────────────────────────────
let rem = null, remErr = null;
const h = bro.vision.loadBirefnet(WEIGHTS, {
    onReady: (m) => { rem = m; },
    onError: (e) => { remErr = e; },
});
assert(h && typeof h.cancel === 'function', 'loadBirefnet async returns a handle');
assert(pumpUntil(() => rem || remErr, 300000), 'load finished');
assert(!remErr, 'load did not error: ' + remErr);
assert(rem.modelSize === 1024, 'default modelSize 1024');
console.log('[smoke] birefnet loaded on ' + rem.device);

// ── decode the sample (sync Image -> canvas -> ImageData) ────────────────────
const img = new Image();
img.src = fs.realpathSync(SAMPLE);
assert(img.naturalWidth > 0, 'sample decoded');
const c = document.createElement('canvas');
c.width = img.naturalWidth; c.height = img.naturalHeight;
const cx = c.getContext('2d');
cx.drawImage(img, 0, 0);
const pixels = cx.getImageData(0, 0, c.width, c.height);

// ── sync removeBackground ────────────────────────────────────────────────────
const t0 = Date.now();
const res = rem.removeBackground(pixels);
console.log('[smoke] matte ' + res.width + 'x' + res.height + ' in ' +
            (Date.now() - t0) + ' ms');
assert(res.width === pixels.width && res.height === pixels.height,
       'matte at input resolution');
assert(res.alpha instanceof Float32Array &&
       res.alpha.length === res.width * res.height, 'alpha shape');
let lo = 1, hi = 0, fg = 0;
for (let i = 0; i < res.alpha.length; i++) {
    const a = res.alpha[i];
    if (a < lo) lo = a;
    if (a > hi) hi = a;
    if (a > 0.5) fg++;
}
const fgFrac = fg / res.alpha.length;
console.log('[smoke] alpha ' + lo.toFixed(3) + '..' + hi.toFixed(3) +
            ', foreground ' + (fgFrac * 100).toFixed(1) + '%');
assert(hi > 0.9 && lo < 0.1, 'matte spans the range (subject + background)');
assert(fgFrac > 0.05 && fgFrac < 0.95, 'subject covers a plausible fraction');
assert(res.matte && res.matte.width === res.width, 'matte bitmap present');
assert(res.image && res.image.width === res.width, 'cutout bitmap present');

// ── async removeBackground + busy guard ─────────────────────────────────────
let asyncRes = null;
rem.removeBackground(pixels, {
    onDone: (r, info) => { asyncRes = { r, info }; },
});
let threw = false;
try { rem.removeBackground(pixels, { onDone: () => {} }); }
catch (e) { threw = true; }
assert(threw, 'concurrent op on one model throws');
assert(pumpUntil(() => asyncRes !== null, 300000), 'async op completed');
assert(!asyncRes.info.error, 'async no error: ' + asyncRes.info.error);
assert(asyncRes.r.alpha.length === res.alpha.length, 'async shape matches');

console.log('[smoke] PASS');
