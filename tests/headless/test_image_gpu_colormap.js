// Test bro.image.gpu.colormap — both uniform-range and autoRange paths.
//
// We construct a field with a known range, render it through a 2-stop
// black→white LUT onto a 'kernel' canvas, and read the composited pixels.
//
// Uniform-range mode: lo/hi are supplied; the lowest source value maps to
// black (0), the highest to white (255).
//
// autoRange mode: the range is a GPU reduction of the field. On the first
// frame the blend is k=1, so the kept range == raw, and we expect the same
// black/white mapping as above without supplying lo/hi.

const root = document.getElementById('root');
const W = 32, H = 32;

root.innerHTML = `<canvas id="cv" width="${W}" height="${H}" style="position:absolute;left:0;top:0;width:${W}px;height:${H}px"></canvas>`;
flush();

const cv = document.getElementById('cv');
if (!bro.kernel.gpu) missingGpuContext('kernel');
else run();

function run() {

// A field that's exactly 0..1 with a diagonal ramp.
const src = new Float32Array(W * H);
for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
        src[y * W + x] = (x + y) / (W + H - 2);  // 0..1
    }
}

// 2-stop black→white LUT.
const lut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 256);

// The red channel at the four corners, from one composite.
function sampleR() {
    const px = getPixels(0, 0, W, H).data;
    return [px[0], px[(W - 1) * 4], px[(H - 1) * W * 4], px[((H - 1) * W + W - 1) * 4]];
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
assertSpansBlackToWhite('uniform');
// Row 0 of the field is the canvas's top row: (0,0) is the field's minimum.
assert(sampleR()[0] < 20, 'field row 0 at the top: top-left is dark');
assert(sampleR()[3] > 235, 'bottom-right is bright');

// ----- autoRange mode -----
// k=1 on the first frame, so the kept range == raw min/max == (0, 1).
bro.image.gpu.colormap(cv, src, lut, { autoRange: true });
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
assertSpansBlackToWhite('autoRange rescaled');

// ----- a constant field should NOT explode (invSpan guard) -----
const src3 = new Float32Array(W * H);
src3.fill(0.5);
bro.image.gpu.colormap(cv, src3, lut, { autoRange: true, ema: 1.0 });
const flatRs = sampleR();
for (let i = 1; i < flatRs.length; i++) {
    // With hi==lo, invSpan is 0 -> t=0 for all pixels -> color = lut[0].
    // The result must be the same across the canvas (no NaN).
    assert(flatRs[i] === flatRs[0],
        'constant field renders uniformly (got mismatched corners: ' + flatRs + ')');
}

// ----- regenerate:false reuses the cached field (no src needed) -----
bro.image.gpu.colormap(cv, src, lut, { autoRange: true, ema: 1.0 });
bro.image.gpu.colormap(cv, null, lut, { regenerate: false, autoRange: true });
assertSpansBlackToWhite('regenerate:false reuses cached field');

// regenerate:false without a prior cache throws.
const fresh = document.createElement('canvas');
fresh.width = W; fresh.height = H;
document.getElementById('root').appendChild(fresh);
let threw = false;
try {
    bro.image.gpu.colormap(fresh, null, lut, { regenerate: false, autoRange: true });
} catch (e) {
    threw = /no cached/.test(e.message);
}
assert(threw, 'colormap: regenerate:false without cache throws');

// A canvas with another context cannot take the colormap.
const other = document.createElement('canvas');
other.width = W; other.height = H;
document.getElementById('root').appendChild(other);
other.getContext('2d');
threw = false;
try {
    bro.image.gpu.colormap(other, src, lut, { lo: 0, hi: 1 });
} catch (e) {
    threw = /'kernel' context/.test(e.message);
}
assert(threw, "colormap: a 2d canvas has no 'kernel' context");

root.innerHTML = '';
}
