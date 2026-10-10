// An image decode never blocks the page thread (docs/file-api.js,
// docs/imagebitmap-api.js): `img.src =` on an 8 MP JPEG returns after a
// header read, `load` / `decode()` follow once a decoder thread finished, and
// createImageBitmap(blob) returns its promise at once and resolves later. A
// second element with the same src reuses the decoded picture.
//
// The yardstick is the same decode run synchronously (bro.image.decodeOriented):
// the page-thread cost of each call must be a small fraction of it.

const fs = require('fs');
const path = require('path');
const os = require('os');

const W = 3264, H = 2448;  // 8 MP
// Noisy rows (a JPEG of flat colour decodes unrealistically fast): one noise
// row, written at a different offset on each line.
const rgb = new Uint8Array(W * H * 3);
const noise = new Uint8Array(W * 3 + 4096);
let seed = 1;
for (let i = 0; i < noise.length; i++) {
    seed = (seed * 1103515245 + 12345) >>> 0;
    noise[i] = seed >>> 24;
}
for (let y = 0; y < H; y++) {
    const off = (y * 37) % 4096;
    rgb.set(noise.subarray(off, off + W * 3), y * W * 3);
}
const jpeg = bro.image.encodeJpeg(rgb, W, H, 3, 90);
assert(jpeg && jpeg.length > 0, 'encoded the 8 MP JPEG');

const dir = path.join(os.tmpdir(), 'bro_img_offthread_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const file = path.join(dir, 'big.jpg');
fs.writeFileSync(file, jpeg);

// The synchronous decode, for scale (best of two).
let decodeMs = Infinity;
for (let i = 0; i < 2; i++) {
    const t = perf.now();
    const d = bro.image.decodeOriented(jpeg);
    decodeMs = Math.min(decodeMs, perf.now() - t);
    assert(d && d.width === W, 'the reference decode worked');
}
const budget = Math.max(4, decodeMs / 4);

// --- <img>: the setter returns before the decode --------------------------
const img = document.createElement('img');
document.body.appendChild(img);
let loads = 0;
img.addEventListener('load', () => loads++);
let t0 = perf.now();
img.src = file.replace(/\\/g, '/');
const setterMs = perf.now() - t0;
console.log('decode ' + decodeMs.toFixed(1) + ' ms; img.src setter ' + setterMs.toFixed(2) + ' ms');
assert(setterMs < budget, 'img.src = 8 MP JPEG blocked the page ' + setterMs.toFixed(1) +
       ' ms (a sync decode takes ' + decodeMs.toFixed(1) + ' ms)');
assert(img.complete === false, 'the image is not complete synchronously');
assert(loads === 0, 'load is not dispatched synchronously');
// Layout already knows the natural size from the header.
assert(img.naturalWidth === W && img.naturalHeight === H,
       'natural size from the header probe: ' + img.naturalWidth + 'x' + img.naturalHeight);
assert(Math.round(img.getBoundingClientRect().width) === W, 'laid out at its natural width before the decode');

await img.decode();
assert(img.complete === true, 'complete after decode()');
await new Promise((r) => setTimeout(r, 0));
assert(loads === 1, 'one load event: ' + loads);

// --- the same src on another element is reused ---------------------------
const img2 = document.createElement('img');
t0 = perf.now();
img2.src = img.src;
const reuseMs = perf.now() - t0;
assert(reuseMs < budget, 'the cached src set in ' + reuseMs.toFixed(1) + ' ms');
assert(img2.complete === true, 'a decoded src is complete at once on another element');
assert(img2.naturalWidth === W, 'reused natural width');

// --- createImageBitmap(blob) ------------------------------------------------
const blob = new Blob([jpeg], { type: 'image/jpeg' });
t0 = perf.now();
const p = createImageBitmap(blob);
const cibMs = perf.now() - t0;
console.log('createImageBitmap(blob) call ' + cibMs.toFixed(2) + ' ms');
assert(cibMs < budget, 'createImageBitmap(8 MP blob) blocked the page ' + cibMs.toFixed(1) + ' ms');
const bmp = await p;
assert(bmp.width === W && bmp.height === H, 'bitmap size ' + bmp.width + 'x' + bmp.height);

// A cropped one, and a bad blob rejects.
const crop = await createImageBitmap(blob, 10, 20, 30, 40);
assert(crop.width === 30 && crop.height === 40, 'cropped bitmap ' + crop.width + 'x' + crop.height);
let rejected = false;
try { await createImageBitmap(new Blob(['nope'], { type: 'image/png' })); } catch (e) { rejected = true; }
assert(rejected, 'a non-image blob rejects');

fs.rmSync(dir, { recursive: true, force: true });
console.log('PASS');
