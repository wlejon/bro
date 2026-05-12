// Test bro.image.gpu.colormap — both uniform-range and autoRange paths.
//
// We construct a noise field with a known range, render it through a
// 2-stop black→white LUT, and read pixels back via getPixel.
//
// Uniform-range mode: lo/hi are supplied; the lowest source value maps to
// black (0), the highest to white (255).
//
// autoRange mode: the engine computes (min, max) via GPU reduction. On the
// first frame the EMA blend k=1 so the smoothed range == raw, and we expect
// the same black/white mapping as above without supplying lo/hi.

const root = document.getElementById('root');
const W = 32, H = 32;

root.innerHTML = `<canvas id="cv" width="${W}" height="${H}" style="width:${W}px;height:${H}px"></canvas>`;
flush();

const cv = document.getElementById('cv');
const gl = cv.getContext('webgl2');
assert(gl, 'webgl2 available');

// Build a noise field that's exactly 0..1 with a diagonal ramp.
const src = new Float32Array(W * H);
for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
        src[y * W + x] = (x + y) / (W + H - 2);  // 0..1
    }
}

// 2-stop black→white LUT.
const lut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 256);

// Read pixels directly from the WebGL canvas backing — bypasses any page
// composite latency between drawArrays and the next flush().
const pxBuf = new Uint8Array(W * H * 4);
function sampleR() {
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
    return [pxBuf[0], pxBuf[(W - 1) * 4], pxBuf[(H - 1) * W * 4], pxBuf[((H - 1) * W + W - 1) * 4]];
}
function assertSpansBlackToWhite(label) {
    const rs = sampleR();
    const mn = Math.min.apply(null, rs);
    const mx = Math.max.apply(null, rs);
    assert(mn < 20, label + ': dark corner present (min r = ' + mn + ')');
    assert(mx > 235, label + ': bright corner present (max r = ' + mx + ')');
}

// ----- uniform-range mode -----
bro.image.gpu.colormap(cv, src, lut, { lo: 0, hi: 1 });
flush();
assertSpansBlackToWhite('uniform');

// ----- autoRange mode -----
// k=1 on the first frame, so the smoothed range == raw min/max == (0, 1).
// We don't pass lo/hi.
bro.image.gpu.colormap(cv, src, lut, { autoRange: true });
flush();
assertSpansBlackToWhite('autoRange first frame');

// Second frame with a narrower field. With ema=1 the new range takes effect
// immediately; the field's min/max should still map to black/white.
const src2 = new Float32Array(W * H);
for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
        src2[y * W + x] = 0.25 + 0.5 * (x + y) / (W + H - 2);  // 0.25..0.75
    }
}
bro.image.gpu.colormap(cv, src2, lut, { autoRange: true, ema: 1.0 });
flush();
assertSpansBlackToWhite('autoRange rescaled');

// ----- a constant field should NOT explode (invSpan guard) -----
const flatVal = 0.5;
const src3 = new Float32Array(W * H);
src3.fill(flatVal);
bro.image.gpu.colormap(cv, src3, lut, { autoRange: true, ema: 1.0 });
flush();
const flatRs = sampleR();
for (let i = 1; i < flatRs.length; i++) {
    // With hi==lo, invSpan is 0 -> t=0 for all pixels -> color = lut[0] = black.
    // Whatever the engine picks, the result must be deterministic across the
    // canvas (no NaN, no flicker between corners).
    assert(flatRs[i] === flatRs[0],
        'constant field renders uniformly (got mismatched corners: ' + flatRs + ')');
}

root.innerHTML = '';
