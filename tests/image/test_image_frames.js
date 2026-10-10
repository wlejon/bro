// bro.image.decodeFrames / openFrames: an animated GIF and an animated WebP
// (libwebp's compositor, registered with broimage by bro) frame by frame —
// canvas size, loop count, delays, exact composed pixels, the caps, stepping
// one frame at a time, reset. docs/image-api.js. Fixtures are written here.

// ── GIF writer: literal LZW codes, a clear code before the table would grow ──
function gif(W, H, gct, loop, frames) {
    const o = [0x47, 0x49, 0x46, 0x38, 0x39, 0x61];
    const u16 = (v) => o.push(v & 255, (v >> 8) & 255);
    const bitsFor = (n) => { let b = 1; while ((1 << b) < n) b++; return b; };
    const table = (t) => { const b = bitsFor(t.length / 3); o.push(...t); for (let i = t.length; i < 3 << b; i++) o.push(0); };
    u16(W); u16(H);
    o.push(0xF0 | (bitsFor(gct.length / 3) - 1), 0, 0);
    table(gct);
    if (loop >= 0) { o.push(0x21, 0xFF, 11, ...[...'NETSCAPE2.0'].map((c) => c.charCodeAt(0)), 3, 1); u16(loop); o.push(0); }
    for (const f of frames) {
        o.push(0x21, 0xF9, 4, ((f.disposal || 0) << 2) | (f.transparent >= 0 ? 1 : 0));
        u16(f.delay || 0);
        o.push(f.transparent >= 0 ? f.transparent : 0, 0, 0x2C);
        u16(f.x || 0); u16(f.y || 0); u16(f.w); u16(f.h);
        o.push(f.lct ? 0x80 | (bitsFor(f.lct.length / 3) - 1) : 0);
        if (f.lct) table(f.lct);
        const min = Math.max(2, bitsFor((f.lct || gct).length / 3)), clear = 1 << min, size = min + 1;
        const bytes = [];
        let acc = 0, n = 0;
        const code = (c) => { acc |= c << n; n += size; while (n >= 8) { bytes.push(acc & 255); acc >>>= 8; n -= 8; } };
        code(clear);
        let since = 0;
        for (const v of f.idx) { if (since === clear - 2) { code(clear); since = 0; } code(v); since++; }
        code(clear + 1);
        if (n) bytes.push(acc & 255);
        o.push(min);
        for (let i = 0; i < bytes.length; i += 255) { const s = bytes.slice(i, i + 255); o.push(s.length, ...s); }
        o.push(0);
    }
    o.push(0x3B);
    return new Uint8Array(o);
}
const fill = (w, h, v) => new Array(w * h).fill(v);

// ── WebP writer (VP8L, as in test_image_webp_svg_decode.js) ─────────────────
function bitWriter() {
    const out = [];
    let acc = 0, n = 0;
    return { put(v, k) { for (let i = 0; i < k; i++) { acc |= ((v >>> i) & 1) << n; if (++n === 8) { out.push(acc); acc = 0; n = 0; } } },
             done() { if (n) out.push(acc); return out; } };
}
function vp8lSolid(w, h, r, g, b, a) {
    const bw = bitWriter();
    const code = (s) => { bw.put(1, 1); bw.put(0, 1); if (s < 2) { bw.put(0, 1); bw.put(s, 1); } else { bw.put(1, 1); bw.put(s, 8); } };
    bw.put(0x2f, 8); bw.put(w - 1, 14); bw.put(h - 1, 14); bw.put(a < 255 ? 1 : 0, 1); bw.put(0, 3);
    bw.put(0, 3);
    code(g); code(r); code(b); code(a); code(0);
    return bw.done();
}
const le24 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255];
const le32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, n >>> 24];
const chunk = (tag, data) => { const c = [...tag].map((ch) => ch.charCodeAt(0)).concat(le32(data.length), data); if (data.length & 1) c.push(0); return c; };
const riff = (chunks) => { const body = [0x57, 0x45, 0x42, 0x50, ...chunks.flat()]; return new Uint8Array([0x52, 0x49, 0x46, 0x46, ...le32(body.length), ...body]); };

const px = (pix, w, x, y) => Array.from(pix.subarray((y * w + x) * 4, (y * w + x) * 4 + 4)).join(',');

// ── GIF: 8x4, three frames ───────────────────────────────────────────────────
// 0: red everywhere, kept. 1: a green 2x2 at (2,1), cleared after (disposal 2).
// 2: a local-palette frame at (0,0) 4x2, its index 1 transparent.
const gct = [255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255];
const g = gif(8, 4, gct, 0, [
    { w: 8, h: 4, idx: fill(8, 4, 0), disposal: 1, delay: 7 },
    { x: 2, y: 1, w: 2, h: 2, idx: fill(2, 2, 1), disposal: 2, delay: 1 },
    { w: 4, h: 2, idx: [0, 1, 0, 1, 1, 0, 1, 0], lct: [255, 0, 255, 0, 0, 0], transparent: 1, delay: 30 },
]);
const all = bro.image.decodeFrames(g);
assert(all && all.width === 8 && all.height === 4, 'decodeFrames: the canvas');
assert(all.frameCount === 3 && all.loopCount === 0 && !all.truncated, 'decodeFrames: counts ' + JSON.stringify({ n: all.frameCount, l: all.loopCount }));
assert(all.frames.map((f) => f.delay).join() === '70,10,300', 'decodeFrames: delays as stored ' + all.frames.map((f) => f.delay));
const [f0, f1, f2] = all.frames.map((f) => f.pixels);
assert(px(f0, 8, 5, 3) === '255,0,0,255', 'frame 0 red');
assert(px(f1, 8, 2, 1) === '0,255,0,255' && px(f1, 8, 1, 1) === '255,0,0,255', 'frame 1: green square over red');
assert(px(f2, 8, 0, 0) === '255,0,255,255', 'frame 2: local palette magenta');
assert(px(f2, 8, 1, 0) === '255,0,0,255', 'frame 2: a transparent index shows red underneath');
assert(px(f2, 8, 2, 1) === '0,0,0,0', 'frame 2: frame 1\'s square disposed to transparent: ' + px(f2, 8, 2, 1));
assert(px(f2, 8, 3, 1) === '255,0,255,255', 'frame 2: magenta drawn over the cleared square');

const two = bro.image.decodeFrames(g, { maxFrames: 2 });
assert(two.frames.length === 2 && two.truncated, 'maxFrames caps the frames');
const oneByBytes = bro.image.decodeFrames(g, { maxBytes: 8 * 4 * 4 * 1.5 });
assert(oneByBytes.frames.length === 1 && oneByBytes.truncated, 'maxBytes caps the frames');

const dec = bro.image.openFrames(g);
assert(dec && dec.frameCount === 3 && dec.loopCount === 0 && dec.delays.join() === '70,10,300', 'openFrames: header');
let f, seen = 0;
while ((f = dec.next())) {
    assert(f.index === seen, 'frames in order');
    assert(px(f.pixels, 8, 0, 0) === px(all.frames[seen].pixels, 8, 0, 0), 'stepping matches decodeFrames at ' + seen);
    seen++;
}
assert(seen === 3 && dec.index === 3, 'openFrames: three frames then null');
assert(dec.reset() && dec.next().index === 0, 'reset() goes back to the first frame');
dec.close();
assert(dec.next() === null, 'close() ends it');

// A still image is one frame; garbage is null.
const still = bro.image.decodeFrames(bro.image.encodePng(new Uint8Array(4 * 4 * 4).fill(200), 4, 4, 4));
assert(still && still.frames.length === 1 && still.frameCount === 1 && still.loopCount === 1, 'a PNG is one frame');
assert(bro.image.openFrames(new Uint8Array([1, 2, 3, 4])) === null, 'garbage opens as null');

// ── Animated WebP: 6x4 canvas; a red full frame, a green 2x2 at (2,2) that
// blends over it, a blue full frame; loop count 3 ────────────────────────────
const anmf = (x, y, w, h, ms, flags, r, gg, b) =>
    chunk('ANMF', [...le24(x / 2), ...le24(y / 2), ...le24(w - 1), ...le24(h - 1), ...le24(ms), flags,
                   ...chunk('VP8L', vp8lSolid(w, h, r, gg, b, 255))]);
const webp = riff([chunk('VP8X', [0x02, 0, 0, 0, ...le24(5), ...le24(3)]), chunk('ANIM', [0, 0, 0, 0, 3, 0]),
                   anmf(0, 0, 6, 4, 100, 0x02, 255, 0, 0),
                   anmf(2, 2, 2, 2, 40, 0x00, 0, 255, 0),
                   anmf(0, 0, 6, 4, 250, 0x02, 0, 0, 255)]);
const w = bro.image.decodeFrames(webp);
assert(w && w.width === 6 && w.height === 4 && w.frameCount === 3 && w.loopCount === 3,
       'an animated WebP: ' + JSON.stringify(w && { w: w.width, h: w.height, n: w.frameCount, l: w.loopCount }));
assert(w.frames.map((x) => x.delay).join() === '100,40,250', 'WebP delays: ' + w.frames.map((x) => x.delay));
assert(px(w.frames[0].pixels, 6, 3, 3) === '255,0,0,255', 'WebP frame 0 red');
assert(px(w.frames[1].pixels, 6, 3, 3) === '0,255,0,255' && px(w.frames[1].pixels, 6, 0, 0) === '255,0,0,255',
       'WebP frame 1: green square composed over red');
assert(px(w.frames[2].pixels, 6, 3, 3) === '0,0,255,255', 'WebP frame 2 blue');
const wd = bro.image.openFrames(webp);
assert(wd && wd.delays.join() === '100,40,250', 'openFrames on WebP: delays from the container');
let wn = 0;
while (wd.next()) wn++;
assert(wn === 3, 'WebP stepped through');
