// bro.image reads what <img> reads: WebP (still, with an EXIF turn, and
// animated) and SVG through decodeOriented / decode / probeDimensions —
// broimage decodes them through the codecs bro registers with it
// (render/image_codecs.h). Before, both came back 0x0 / 1x1 and the probe
// failed. Every fixture is written here.

// ── A lossless WebP writer (VP8L, no transforms, simple prefix codes) ───────
// Each image has one red, blue and alpha, and one or two greens: a pixel is
// one bit (0: the smaller green). Enough for solid frames and two-tone tests.
function bitWriter() {
    const out = [];
    let acc = 0, n = 0;
    return {
        put(v, k) {
            for (let i = 0; i < k; i++) {
                acc |= ((v >>> i) & 1) << n;
                if (++n === 8) { out.push(acc); acc = 0; n = 0; }
            }
        },
        done() { if (n) out.push(acc); return out; },
    };
}
function simpleCode(bw, syms) {
    bw.put(1, 1);                    // simple code
    bw.put(syms.length - 1, 1);
    if (syms[0] < 2) { bw.put(0, 1); bw.put(syms[0], 1); } else { bw.put(1, 1); bw.put(syms[0], 8); }
    if (syms.length === 2) bw.put(syms[1], 8);
}
function vp8l(w, h, r, b, a, greens, bits) {
    const bw = bitWriter();
    bw.put(0x2f, 8); bw.put(w - 1, 14); bw.put(h - 1, 14); bw.put(a < 255 ? 1 : 0, 1); bw.put(0, 3);
    bw.put(0, 1); bw.put(0, 1); bw.put(0, 1);  // no transform, no colour cache, no meta codes
    simpleCode(bw, greens); simpleCode(bw, [r]); simpleCode(bw, [b]); simpleCode(bw, [a]); simpleCode(bw, [0]);
    if (greens.length === 2) for (const s of bits) bw.put(s, 1);
    return bw.done();
}
const le24 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255];
const le32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24];
function chunk(tag, data) {
    const c = [...tag].map((ch) => ch.charCodeAt(0)).concat(le32(data.length), data);
    if (data.length & 1) c.push(0);
    return c;
}
const riff = (chunks) => {
    const body = [0x57, 0x45, 0x42, 0x50, ...chunks.flat()];
    return new Uint8Array([0x52, 0x49, 0x46, 0x46, ...le32(body.length), ...body]);
};

// 4x2: left half red (green 0), right half yellow (green 255).
const twoTone = vp8l(4, 2, 255, 0, 255, [0, 255], [0, 0, 1, 1, 0, 0, 1, 1]);
const stillWebp = riff([chunk('VP8L', twoTone)]);

// The same with EXIF Orientation 6 (rotate 90 degrees clockwise to display).
const exif = [0x49, 0x49, 0x2A, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0];
const turnedWebp = riff([chunk('VP8X', [0x08, 0, 0, 0, ...le24(3), ...le24(1)]),
                         chunk('VP8L', twoTone), chunk('EXIF', exif)]);

// Animated, 6x4 canvas: red, green, blue frames of 100 / 50 / 200 ms, played twice.
const frame = (r, g, b, ms) => chunk('ANMF', [...le24(0), ...le24(0), ...le24(5), ...le24(3), ...le24(ms), 0x02,
                                              ...chunk('VP8L', vp8l(6, 4, r, b, 255, [g], []))]);
const animWebp = riff([chunk('VP8X', [0x02, 0, 0, 0, ...le24(5), ...le24(3)]),
                       chunk('ANIM', [0, 0, 0, 0, 2, 0]),
                       frame(255, 0, 0, 100), frame(0, 255, 0, 50), frame(0, 0, 255, 200)]);

const px = (d, x, y) => Array.from(d.pixels.subarray((y * d.width + x) * 4, (y * d.width + x) * 4 + 4)).join(',');

// ── WebP ─────────────────────────────────────────────────────────────────────
let p = bro.image.probeDimensions(stillWebp);
assert(p && p.width === 4 && p.height === 2, 'probeDimensions reads a WebP: ' + JSON.stringify(p));
let d = bro.image.decodeOriented(stillWebp);
assert(d.width === 4 && d.height === 2 && d.pixels.length === 32, 'decodeOriented decodes a WebP: ' + d.width + 'x' + d.height);
assert(px(d, 0, 0) === '255,0,0,255' && px(d, 3, 1) === '255,255,0,255', 'the WebP pixels: ' + px(d, 0, 0) + ' / ' + px(d, 3, 1));

assert(bro.image.readExifOrientation(turnedWebp) === 6, 'the WebP carries orientation 6');
p = bro.image.probeDimensions(turnedWebp);
assert(p && p.width === 4 && p.height === 2, 'probe of the extended WebP: ' + JSON.stringify(p));
d = bro.image.decodeOriented(turnedWebp);
assert(d.width === 2 && d.height === 4, 'decodeOriented turns the WebP upright: ' + d.width + 'x' + d.height);
assert(px(d, 0, 0) === '255,0,0,255' && px(d, 1, 1) === '255,0,0,255', 'upright: red on top, ' + px(d, 0, 0));
assert(px(d, 0, 3) === '255,255,0,255', 'upright: yellow below, ' + px(d, 0, 3));

p = bro.image.probeDimensions(animWebp);
assert(p && p.width === 6 && p.height === 4, 'probe of an animated WebP is its canvas: ' + JSON.stringify(p));
d = bro.image.decodeOriented(animWebp);
assert(d.width === 6 && d.height === 4 && px(d, 2, 2) === '255,0,0,255', 'an animated WebP decodes as its first frame: ' + px(d, 2, 2));

// ── SVG ──────────────────────────────────────────────────────────────────────
const enc = (s) => new Uint8Array([...s].map((c) => c.charCodeAt(0)));
const svg = enc('<svg xmlns="http://www.w3.org/2000/svg" width="10" height="6">' +
                '<rect width="5" height="6" fill="#0000ff"/><rect x="5" width="5" height="6" fill="#00ff00"/></svg>');
p = bro.image.probeDimensions(svg);
assert(p && p.width === 10 && p.height === 6 && p.channels === 4, 'probeDimensions reads an SVG: ' + JSON.stringify(p));
d = bro.image.decodeOriented(svg);
assert(d.width === 10 && d.height === 6, 'decodeOriented rasterizes an SVG at its size: ' + d.width + 'x' + d.height);
assert(px(d, 1, 3) === '0,0,255,255' && px(d, 8, 3) === '0,255,0,255', 'the SVG pixels: ' + px(d, 1, 3) + ' / ' + px(d, 8, 3));
const viewBoxOnly = enc('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 4"><rect width="8" height="4" fill="red"/></svg>');
p = bro.image.probeDimensions(viewBoxOnly);
assert(p && p.width === 8 && p.height === 4, 'a viewBox-only SVG probes at the viewBox: ' + JSON.stringify(p));

// From a file too (the path entry point), on the page and in a worker.
const fs = require('fs');
const path = require('path');
const os = require('os');
const dir = path.join(os.tmpdir(), 'bro_image_webp_svg_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
const webpFile = path.join(dir, 'turned.webp').replace(/\\/g, '/');
fs.writeFileSync(webpFile, turnedWebp);
d = bro.image.decodeOriented(webpFile);
assert(d.width === 2 && d.height === 4, 'decodeOriented(path) of a WebP: ' + d.width + 'x' + d.height);

const workerFile = path.join(dir, 'w.js').replace(/\\/g, '/');
fs.writeFileSync(workerFile, `
    self.onmessage = (e) => {
        const d = bro.image.decodeOriented(e.data.path);
        const p = bro.image.probeDimensions(e.data.svg);
        self.postMessage({ w: d.width, h: d.height, sw: p && p.width, sh: p && p.height });
    };`);
const worker = new Worker(workerFile);
const got = await new Promise((resolve) => {
    worker.onmessage = (e) => resolve(e.data);
    worker.postMessage({ path: webpFile, svg });
});
worker.terminate();
assert(got.w === 2 && got.h === 4 && got.sw === 10 && got.sh === 6, 'a worker reads WebP and SVG: ' + JSON.stringify(got));
fs.rmSync(dir, { recursive: true, force: true });
