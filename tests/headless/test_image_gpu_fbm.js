// Test bro.image.gpu.fbm2D — GLSL Simplex FBm rendered straight to canvas,
// bypassing the CPU-side noise gen and upload.

const W = 64, H = 64;
document.getElementById('root').innerHTML =
    `<canvas id="cv" width="${W}" height="${H}" style="width:${W}px;height:${H}px"></canvas>`;
flush();

const cv = document.getElementById('cv');
const gl = cv.getContext('webgl2');
assert(gl, 'webgl2 available');

const lut = bro.image.gradient([[0, 0, 0, 0], [1, 255, 255, 255]], 256);
const pxBuf = new Uint8Array(W * H * 4);

function corners() {
    gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
    return [pxBuf[0], pxBuf[(W - 1) * 4],
            pxBuf[(H - 1) * W * 4], pxBuf[((H - 1) * W + W - 1) * 4]];
}

// 1) Single-octave Simplex at a chunky frequency: the field naturally spans
//    its full range across a 64×64 patch. autoRange + ema=1 means the LUT
//    maps min→black and max→white this frame, so we expect dark and bright
//    samples on the canvas.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05,
    octaves: 1,
    seed: 1337,
    autoRange: true, ema: 1.0,
});
flush();
let rs = corners();
let mn = Math.min.apply(null, rs);
let mx = Math.max.apply(null, rs);
// Single octave at one offset can be unlucky — also peek at the histogram
// of the entire 64×64 to be sure.
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
let minR = 255, maxR = 0;
for (let i = 0; i < W * H; i++) {
    const r = pxBuf[i * 4];
    if (r < minR) minR = r;
    if (r > maxR) maxR = r;
}
assert(minR < 20, 'fbm2D: black appears (got min ' + minR + ')');
assert(maxR > 235, 'fbm2D: white appears (got max ' + maxR + ')');

// 2) Different seed → visibly different field. Fingerprint the first row
//    and assert it changes when we change the seed.
function rowFingerprint() {
    gl.readPixels(0, H / 2, W, 1, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
    let s = 0;
    for (let i = 0; i < W; i++) s = (s * 31 + pxBuf[i * 4]) | 0;
    return s;
}
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337, autoRange: true, ema: 1.0,
});
flush();
const fpA = rowFingerprint();
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 9999, autoRange: true, ema: 1.0,
});
flush();
const fpB = rowFingerprint();
assert(fpA !== fpB, 'fbm2D: seed change shifts the field');

// 3) Multi-octave FBm runs without error and produces non-constant output.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.02, octaves: 8, gain: 0.5, lacunarity: 2.0,
    seed: 1337, autoRange: true, ema: 1.0,
});
flush();
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
let r0 = pxBuf[0], differs = false;
for (let i = 1; i < W * H; i++) {
    if (pxBuf[i * 4] !== r0) { differs = true; break; }
}
assert(differs, 'fbm2D 8-octave: output is not constant');

// 4) Uniform-range mode (no autoRange): the LUT maps the explicit range.
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337,
    lo: -1.0, hi: 1.0,   // simplex range roughly [-1, 1]
});
flush();
gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, pxBuf);
let any = false;
for (let i = 0; i < W * H; i++) {
    if (pxBuf[i * 4] > 0 && pxBuf[i * 4] < 255) { any = true; break; }
}
assert(any, 'fbm2D uniform-range: some midtone pixel present');

// 5) Unsupported type throws clearly.
let threw = false;
try {
    bro.image.gpu.fbm2D(cv, lut, { frequency: 0.05, type: 'Perlin' });
} catch (e) {
    threw = /not implemented/.test(e.message);
}
assert(threw, 'fbm2D: unsupported type throws "not implemented"');

// 6) viewRect: pre-render a wider tile, then slide a window across it.
// Sized tile (3W × H), regenerated once. Each subsequent fbm2D call with
// regenerate:false slides a viewRect across without rerunning the FBm.
const TILE_W = W * 3;
bro.image.gpu.fbm2D(cv, lut, {
    frequency: 0.05, octaves: 1, seed: 1337,
    srcW: TILE_W, srcH: H,
    autoRange: true, ema: 1.0,
    viewRect: { x: 0, y: 0, w: W, h: H },
});
flush();
const fpAt0 = rowFingerprint();

// Same tile, slide to x = W (one full canvas width across).
bro.image.gpu.fbm2D(cv, lut, {
    regenerate: false,
    autoRange: true, ema: 1.0,
    viewRect: { x: W, y: 0, w: W, h: H },
});
flush();
const fpAtW = rowFingerprint();
assert(fpAt0 !== fpAtW,
    'viewRect slide reveals different sub-region of the tile (' +
    fpAt0 + ' vs ' + fpAtW + ')');

// Same tile, slide back to x=0 — should reproduce the original fingerprint
// (the underlying noiseTex is unchanged; only the viewport moves).
bro.image.gpu.fbm2D(cv, lut, {
    regenerate: false,
    autoRange: true, ema: 1.0,
    viewRect: { x: 0, y: 0, w: W, h: H },
});
flush();
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
