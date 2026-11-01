// Test bro.image.gpu.fbm2D — Simplex FBm made by a brokernel kernel on the
// GPU and coloured onto a 'kernel' canvas, with no CPU noise or upload.

const W = 64, H = 64;
document.getElementById('root').innerHTML =
    `<canvas id="cv" width="${W}" height="${H}" style="position:absolute;left:0;top:0;width:${W}px;height:${H}px"></canvas>`;
flush();

const cv = document.getElementById('cv');
if (!bro.kernel.gpu) missingGpuContext('kernel');
else run();

function run() {

const lut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 256);
const pixels = () => getPixels(0, 0, W, H).data;

// 1) Single-octave Simplex at a chunky frequency: the field spans its full
//    range across a 64×64 patch. autoRange + ema=1 maps min→black and
//    max→white this frame, so dark and bright pixels are on the canvas.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05,
    octaves: 1,
    seed: 1337,
    autoRange: true, ema: 1.0,
});
let px = pixels();
let minR = 255, maxR = 0;
for (let i = 0; i < W * H; i++) {
    const r = px[i * 4];
    if (r < minR) minR = r;
    if (r > maxR) maxR = r;
}
assert(minR < 20, 'fbm2D: black appears (got min ' + minR + ')');
assert(maxR > 235, 'fbm2D: white appears (got max ' + maxR + ')');

// 2) Different seed → visibly different field. Fingerprint the middle row
//    and assert it changes when we change the seed.
function rowFingerprint() {
    const row = getPixels(0, H / 2, W, 1).data;
    let s = 0;
    for (let i = 0; i < W; i++) s = (s * 31 + row[i * 4]) | 0;
    return s;
}
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337, autoRange: true, ema: 1.0,
});
const fpA = rowFingerprint();
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 9999, autoRange: true, ema: 1.0,
});
const fpB = rowFingerprint();
assert(fpA !== fpB, 'fbm2D: seed change shifts the field');

// 3) Multi-octave FBm runs without error and produces non-constant output.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.02, octaves: 8, gain: 0.5, lacunarity: 2.0,
    seed: 1337, autoRange: true, ema: 1.0,
});
px = pixels();
let differs = false;
for (let i = 1; i < W * H; i++) {
    if (px[i * 4] !== px[0]) { differs = true; break; }
}
assert(differs, 'fbm2D 8-octave: output is not constant');

// 4) Uniform-range mode (no autoRange): the LUT maps the explicit range.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337,
    lo: -1.0, hi: 1.0,   // simplex range roughly [-1, 1]
});
px = pixels();
let any = false;
for (let i = 0; i < W * H; i++) {
    if (px[i * 4] > 0 && px[i * 4] < 255) { any = true; break; }
}
assert(any, 'fbm2D uniform-range: some midtone pixel present');

// 5) Unsupported type and octave count throw clearly.
let threw = false;
try {
    bro.image.gpu.fbm2D(cv, lut, { frequency: 0.05, type: 'Perlin' });
} catch (e) {
    threw = /not implemented/.test(e.message);
}
assert(threw, 'fbm2D: unsupported type throws "not implemented"');
threw = false;
try {
    bro.image.gpu.fbm2D(cv, lut, { frequency: 0.05, octaves: 17 });
} catch (e) {
    threw = e instanceof RangeError;
}
assert(threw, 'fbm2D: octaves past 16 throw a RangeError');

// 6) viewRect: pre-render a wider tile, then slide a window across it.
// A 3W × H tile, made once; each regenerate:false call slides the viewRect
// across it without running the FBm again.
const TILE_W = W * 3;
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337,
    srcW: TILE_W, srcH: H,
    autoRange: true, ema: 1.0,
    viewRect: { x: 0, y: 0, w: W, h: H },
});
const fpAt0 = rowFingerprint();

bro.image.gpu.fbm2D(cv, lut, {
    regenerate: false,
    autoRange: true, ema: 1.0,
    viewRect: { x: W, y: 0, w: W, h: H },
});
const fpAtW = rowFingerprint();
assert(fpAt0 !== fpAtW,
    'viewRect slide reveals different sub-region of the tile (' +
    fpAt0 + ' vs ' + fpAtW + ')');

// Back to x=0 reproduces the first view: the field is unchanged.
bro.image.gpu.fbm2D(cv, lut, {
    regenerate: false,
    autoRange: true, ema: 1.0,
    viewRect: { x: 0, y: 0, w: W, h: H },
});
const fpAt0Again = rowFingerprint();
assert(fpAt0 === fpAt0Again,
    'regenerate:false leaves the tile intact across slides');

// 7) regenerate:false without a prior cached field throws clearly.
const fresh = document.createElement('canvas');
fresh.width = W; fresh.height = H;
document.getElementById('root').appendChild(fresh);
let regenThrew = false;
try {
    bro.image.gpu.fbm2D(fresh, lut, {
        regenerate: false, autoRange: true,
    });
} catch (e) {
    regenThrew = /no cached noise field/.test(e.message);
}
assert(regenThrew, 'fbm2D: regenerate:false without cache throws');

document.getElementById('root').innerHTML = '';
}
