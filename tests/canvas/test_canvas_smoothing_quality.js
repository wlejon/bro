// ctx.imageSmoothingQuality ('low' | 'medium' | 'high') alongside
// imageSmoothingEnabled: the enum attribute, the state stack, and what each
// level does to a magnified image, read back with getImageData.

function makeCanvas(w, h) {
    const c = document.createElement('canvas');
    c.setAttribute('width', String(w));
    c.setAttribute('height', String(h));
    document.body.appendChild(c);
    flush();
    return c;
}

function near(a, b, tol) { return Math.abs(a - b) <= tol; }

const cv = makeCanvas(20, 1);
const ctx = cv.getContext('2d');

// ---------------------------------------------------------------------------
// The attribute
// ---------------------------------------------------------------------------
assert(ctx.imageSmoothingQuality === 'low', 'imageSmoothingQuality defaults to "low", got ' + ctx.imageSmoothingQuality);
ctx.imageSmoothingQuality = 'high';
assert(ctx.imageSmoothingQuality === 'high', 'set to "high"');
ctx.imageSmoothingQuality = 'HIGH';
assert(ctx.imageSmoothingQuality === 'high', 'a value outside the enum is ignored');
ctx.imageSmoothingQuality = 'bogus';
assert(ctx.imageSmoothingQuality === 'high', '"bogus" is ignored');
ctx.save();
ctx.imageSmoothingQuality = 'medium';
assert(ctx.imageSmoothingQuality === 'medium', 'set to "medium" inside save');
ctx.restore();
assert(ctx.imageSmoothingQuality === 'high', 'restore brings the quality back');
ctx.imageSmoothingQuality = 'low';

// ---------------------------------------------------------------------------
// A 2x1 black|white image magnified 10x across a 20x1 canvas
// ---------------------------------------------------------------------------
const src = makeCanvas(2, 1);
const sctx = src.getContext('2d');
sctx.fillStyle = 'black';
sctx.fillRect(0, 0, 1, 1);
sctx.fillStyle = 'white';
sctx.fillRect(1, 0, 1, 1);

function row(setup) {
    ctx.save();
    setup();
    ctx.clearRect(0, 0, 20, 1);
    ctx.drawImage(src, 0, 0, 20, 1);
    ctx.restore();
    const d = ctx.getImageData(0, 0, 20, 1).data;
    const out = [];
    for (let i = 0; i < 20; i++) out.push(d[i * 4]);
    return out;
}

const off = row(() => { ctx.imageSmoothingEnabled = false; });
const low = row(() => { ctx.imageSmoothingQuality = 'low'; });
const medium = row(() => { ctx.imageSmoothingQuality = 'medium'; });
const high = row(() => { ctx.imageSmoothingQuality = 'high'; });
const offHigh = row(() => { ctx.imageSmoothingEnabled = false; ctx.imageSmoothingQuality = 'high'; });

// Smoothing off: nearest neighbour, a hard step at x = 10 whatever the quality.
assert(off[9] === 0 && off[10] === 255, 'smoothing off: hard step, got ' + off[9] + ',' + off[10]);
assert(offHigh[9] === 0 && offHigh[10] === 255, 'smoothing off ignores the quality');

// low: bilinear. Pixel x samples source u = (x + 0.5) / 10; between the texel
// centres at 0.5 and 1.5 that is a straight ramp — x = 5 -> 13, x = 7 -> 64,
// x = 12 -> 191 — and the clamped ends are flat.
assert(near(low[5], 13, 3), 'low: x=5 ~13, got ' + low[5]);
assert(near(low[7], 64, 3), 'low: x=7 ~64, got ' + low[7]);
assert(near(low[12], 191, 3), 'low: x=12 ~191, got ' + low[12]);
assert(low[2] === 0 && low[17] === 255, 'low: flat beyond the texel centres, got ' + low[2] + ',' + low[17]);

// medium: bilinear with mipmaps, which only matter when minifying, so a
// magnification reads the same as low.
for (let x = 0; x < 20; x++) {
    assert(near(medium[x], low[x], 2), `medium == low when magnifying at x=${x}: ${medium[x]} vs ${low[x]}`);
}

// high: a cubic resampler — not the straight bilinear ramp. Near the texel
// centre it reaches past the neighbouring texel (x = 4 is flat black under
// bilinear and not under the cubic), and it bends the ramp (x = 5).
let diff = 0;
for (let x = 0; x < 20; x++) diff += Math.abs(high[x] - low[x]);
assert(diff >= 15, 'high differs from low across the row, total ' + diff + ' (' + high + ' vs ' + low + ')');
assert(high[9] > 60 && high[9] < 200 && high[10] > 60 && high[10] < 200, 'high still interpolates across the edge');
