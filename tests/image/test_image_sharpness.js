// bro.image.sharpness: focus measures over a region (broimage sharpness.h).
// Exact values on a step edge, a checkerboard the Laplacian sees and Sobel
// does not, a blurred copy measuring lower, rect = a cropped copy, grey RGB =
// luma, a decoded image, any thread count the same, bad arguments.

assert(typeof bro.image.sharpness === 'function', 'bro.image.sharpness');
const I = bro.image;

function grey(w, h, f, ch) {
    ch = ch || 1;
    const px = new Uint8Array(w * h * ch);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const v = f(x, y), i = (y * w + x) * ch;
        px[i] = v;
        if (ch >= 3) { px[i + 1] = v; px[i + 2] = v; }
        if (ch === 4) px[i + 3] = 255;
        if (ch === 2) px[i + 1] = 255;
    }
    return px;
}
const same = (a, b) => a.laplacianVar === b.laplacianVar && a.tenengrad === b.tenengrad &&
    a.lumaMean === b.lumaMean && a.lumaStd === b.lumaStd && a.pixels === b.pixels;

// Step edge 0 | 200 at column 20: Laplacian -200 / +200 and Sobel gx 800 on
// the two columns beside it, zero elsewhere.
{
    const W = 50, H = 30;
    const r = I.sharpness(grey(W, H, (x) => (x < 20 ? 0 : 200)), { width: W, height: H, channels: 1 });
    const n = (W - 2) * (H - 2);
    assert(r.pixels === n, 'pixels = interior count');
    assert(Math.abs(r.laplacianVar - 2 * (H - 2) * 40000 / n) < 1e-9, 'step laplacianVar ' + r.laplacianVar);
    assert(Math.abs(r.tenengrad - 2 * (H - 2) * 640000 / n) < 1e-9, 'step tenengrad ' + r.tenengrad);
}

// One-pixel checkerboard: Laplacian +-1020, Sobel nothing.
{
    const r = I.sharpness(grey(32, 32, (x, y) => ((x + y) & 1 ? 255 : 0)), { width: 32, height: 32, channels: 1 });
    assert(r.laplacianVar === 1020 * 1020, 'checker laplacianVar ' + r.laplacianVar);
    assert(r.tenengrad === 0, 'checker tenengrad ' + r.tenengrad);
    assert(r.lumaMean === 127.5, 'checker lumaMean ' + r.lumaMean);
}

// A textured image against its box blur; rect against a cropped copy; grey
// RGBA / RGB / luma+alpha against luma; thread counts.
{
    const W = 240, H = 160;
    let s = 7;
    const tex = (x, y) => {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return Math.max(0, Math.min(255, Math.round(128 + 50 * Math.sin(x * 0.31) * Math.cos(y * 0.23) + ((s >>> 24) & 63) - 32)));
    };
    const img = grey(W, H, tex);
    const soft = new Uint8Array(img.length);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        let sum = 0, k = 0;
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
            const xx = Math.min(W - 1, Math.max(0, x + dx)), yy = Math.min(H - 1, Math.max(0, y + dy));
            sum += img[yy * W + xx]; k++;
        }
        soft[y * W + x] = Math.round(sum / k);
    }
    const a = I.sharpness(img, { width: W, height: H, channels: 1 });
    const b = I.sharpness(soft, { width: W, height: H, channels: 1 });
    assert(a.laplacianVar > 4 * b.laplacianVar, 'blur lowers laplacianVar: ' + a.laplacianVar + ' vs ' + b.laplacianVar);
    assert(a.tenengrad > 1.5 * b.tenengrad, 'blur lowers tenengrad: ' + a.tenengrad + ' vs ' + b.tenengrad);

    const rect = { x: 31, y: 12, w: 97, h: 70 };
    const crop = new Uint8Array(rect.w * rect.h);
    for (let y = 0; y < rect.h; y++) crop.set(img.subarray((y + rect.y) * W + rect.x, (y + rect.y) * W + rect.x + rect.w), y * rect.w);
    assert(same(I.sharpness(img, { width: W, height: H, channels: 1, rect }),
                I.sharpness(crop, { width: rect.w, height: rect.h, channels: 1 })), 'rect = cropped copy');

    const rgba = new Uint8Array(W * H * 4), rgb = new Uint8Array(W * H * 3), la = new Uint8Array(W * H * 2);
    for (let i = 0; i < W * H; i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = img[i]; rgba[i * 4 + 3] = 255;
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = img[i];
        la[i * 2] = img[i]; la[i * 2 + 1] = 9;
    }
    assert(same(a, I.sharpness(rgba, { width: W, height: H })), 'grey RGBA (default channels) = luma');
    assert(same(a, I.sharpness(rgb, { width: W, height: H, channels: 3 })), 'grey RGB = luma');
    assert(same(a, I.sharpness(la, { width: W, height: H, channels: 2 })), 'luma + alpha = luma');
    for (const threads of [1, 2, 5, 8])
        assert(same(a, I.sharpness(img, { width: W, height: H, channels: 1, threads })), 'threads ' + threads + ' agree');
}

// A decoded image measures as it comes from decodeScaled, at any decode size.
{
    const W = 128, H = 96;
    const px = grey(W, H, (x, y) => (((x >> 3) + (y >> 3)) & 1 ? 230 : 20), 4);
    const png = I.encodePng(px, W, H, 4);
    assert(png && png.length, 'encodePng');
    const d = I.decodeScaled(png, {});
    assert(d.width === W && d.height === H, 'decoded ' + d.width + 'x' + d.height);
    const r = I.sharpness(d.pixels, { width: d.width, height: d.height, channels: d.channels });
    assert(r.tenengrad > 0 && r.laplacianVar > 0, 'decoded image measures');
    const small = I.decodeScaled(png, { width: W / 4 });
    const q = I.sharpness(small.pixels, { width: small.width, height: small.height, channels: small.channels });
    assert(q.pixels === (small.width - 2) * (small.height - 2), 'scaled decode pixels');
}

// Empty rect, out-of-range arguments.
{
    const px = new Uint8Array(20 * 20 * 4);
    assert(I.sharpness(px, { width: 20, height: 20, rect: { x: 0, y: 0, w: 0, h: 9 } }).pixels === 0, 'empty rect');
    assert(I.sharpness(px, { width: 20, height: 20, rect: { x: 18, y: 0, w: 9, h: 9 } }).pixels === 0, 'rect under 3 wide');
    let threw = null;
    try { I.sharpness(px, { width: 20, height: 21 }); } catch (e) { threw = e; }
    assert(threw instanceof RangeError, 'past the end of src throws RangeError');
    threw = null;
    try { I.sharpness(new Float32Array(16), { width: 2, height: 2 }); } catch (e) { threw = e; }
    assert(threw instanceof TypeError, 'Float32Array src throws TypeError');
}

console.log('PASS: bro.image.sharpness');
