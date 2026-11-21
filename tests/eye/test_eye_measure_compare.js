// Test bro.eye.measure, bro.eye.compare, bro.eye.visualize, and Worker support.

assert(bro.eye && bro.eye.available === true, 'bro.eye is available');

const W = 128, H = 128;

function makeImageData(fillFn) {
    const data = new Uint8ClampedArray(W * H * 4);
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const [r, g, b] = fillFn(x, y);
            const i = (y * W + x) * 4;
            data[i] = r; data[i + 1] = g; data[i + 2] = b; data[i + 3] = 255;
        }
    }
    return { width: W, height: H, data };
}

// 1. Solid black / white exposure checks
const black = makeImageData(() => [0, 0, 0]);
const white = makeImageData(() => [255, 255, 255]);
const mBlack = bro.eye.measure(black);
const mWhite = bro.eye.measure(white);
assert(mBlack.crushedBlackRatio > 0.99, 'crushed blacks detected');
assert(mWhite.blownWhiteRatio > 0.99, 'blown whites detected');
assert(mWhite.lumaMean > 0.99 && mBlack.lumaMean < 0.01, 'Oklab lumaMean matches extremes');

// 2. Banding vs smooth gradient
const banded = makeImageData((x) => {
    const step = Math.floor((x * 8) / W);
    const v = 50 + step * 3;
    return [v, v, v];
});
const smooth = makeImageData((x) => {
    const v = Math.floor((x * 255) / (W - 1));
    return [v, v, v];
});
const mBand = bro.eye.measure(banded);
const mSmooth = bro.eye.measure(smooth);
assert(mBand.bandingScore > mSmooth.bandingScore + 0.3,
    `banding should be much higher on stepped gradient (${mBand.bandingScore} vs ${mSmooth.bandingScore})`);

// 3. 2D FFT tiling detector
const tiled = makeImageData((x, y) => {
    const s = 0.5 + 0.42 * Math.cos((2 * Math.PI * x) / 16) * Math.cos((2 * Math.PI * y) / 16);
    const v = Math.max(0, Math.min(255, Math.round(s * 255)));
    return [v, v, v];
});
const mTiled = bro.eye.measure(tiled);
assert(mTiled.tiling.score > 0.4, `tiling score > 0.4 (${mTiled.tiling.score})`);
assert(mSmooth.tiling.score < 0.15, `smooth gradient tiling < 0.15 (${mSmooth.tiling.score})`);
assert(mTiled.seam.score < 0.08, `periodic sine seam score < 0.08 (${mTiled.seam.score})`);
assert(mSmooth.seam.score > 0.5, `linear gradient seam score > 0.5 (${mSmooth.seam.score})`);
assert(Math.abs(mTiled.detailBands.low + mTiled.detailBands.mid + mTiled.detailBands.high - 1.0) < 1e-4,
    'detailBands sum to 1');

// 4. Compare metrics & determinism
const cSame = bro.eye.compare(tiled, tiled);
assert(cSame.score === 0 && cSame.ssim === 1 && cSame.deltaEMean === 0, 'self-compare is exact 0');

const dark = makeImageData(() => [45, 45, 45]);
const bright = makeImageData(() => [210, 210, 210]);
const cDiff = bro.eye.compare(bright, dark);
assert(cDiff.lumaDiff > 0.4, 'lumaDiff is positive for brighter candidate');
assert(cDiff.deltaEMean > 40, 'deltaEMean reflects large perceptual difference');

const os = require('os');
const path = require('path');

// 5. Visualize modes + PNG export + reload by path
const pngPath = path.join(os.tmpdir(), 'eye_test_diff.png').replace(/\\/g, '/');
const vis = bro.eye.visualize([bright, dark], {
    mode: 'diff',
    labels: ['bright', 'dark'],
    path: pngPath,
});
assert(vis.width > W * 2 && vis.height > H, 'diff visualization dimensions');
assert(vis.panels.length === 3, 'diff visualization has 3 panels');
assert(vis.path === pngPath, 'PNG written cleanly');

const mFromPath = bro.eye.measure(pngPath);
assert(mFromPath.width === vis.width && mFromPath.height === vis.height, 'measure(path) decodes saved PNG');

// 6. Worker realm support
let workerDone = false;
const worker = new Worker('../eye/worker_eye.js');
worker.onmessage = (e) => {
    const d = e.data;
    assert(d.available === true, 'Worker sees bro.eye.available === true');
    assert(d.hasAudit === 'undefined', 'Worker omits main-realm audit');
    assert(d.selfScore === 0 && d.selfSsim === 1, 'Worker compare matches');
    workerDone = true;
};
worker.postMessage({ width: W, height: H, pixels: tiled.data.buffer });

for (let i = 0; i < 300 && !workerDone; i++) {
    advanceTime(16);
    wallSleep(10);
}
assert(workerDone, 'Worker completed bro.eye measurement');
worker.terminate();
