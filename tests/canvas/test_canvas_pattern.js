// ctx.createPattern(): CanvasPattern as fillStyle / strokeStyle, the four
// repetitions, CanvasPattern.setTransform, the image sources, and the spec's
// errors. Asserted on pixels read back with getImageData.

function makeCanvas(w, h) {
    const c = document.createElement('canvas');
    c.setAttribute('width', String(w));
    c.setAttribute('height', String(h));
    document.body.appendChild(c);
    flush();
    return c;
}

function px(ctx, x, y) {
    const d = ctx.getImageData(x, y, 1, 1).data;
    return [d[0], d[1], d[2], d[3]];
}

function near(a, b, tol) { return Math.abs(a - b) <= tol; }

function isRed(p)   { return p[0] > 240 && p[1] < 15 && p[2] < 15 && p[3] > 240; }
function isBlue(p)  { return p[0] < 15 && p[1] < 15 && p[2] > 240 && p[3] > 240; }
function isClear(p) { return p[3] === 0; }

// A 2x2 checker: red on the diagonal, blue off it.
const src = makeCanvas(2, 2);
const sctx = src.getContext('2d');
sctx.fillStyle = 'red';
sctx.fillRect(0, 0, 1, 1);
sctx.fillRect(1, 1, 1, 1);
sctx.fillStyle = 'blue';
sctx.fillRect(1, 0, 1, 1);
sctx.fillRect(0, 1, 1, 1);

const dst = makeCanvas(10, 10);
const ctx = dst.getContext('2d');

// ---------------------------------------------------------------------------
// repeat: the checker tiles across the whole rect
// ---------------------------------------------------------------------------
const pat = ctx.createPattern(src, 'repeat');
assert(pat !== null && typeof pat === 'object', 'createPattern returns an object');
assert(pat instanceof CanvasPattern, 'pattern is a CanvasPattern');
assert(typeof pat.setTransform === 'function', 'CanvasPattern has setTransform');

ctx.fillStyle = pat;
assert(ctx.fillStyle === pat, 'fillStyle answers the pattern object back');
ctx.fillRect(0, 0, 10, 10);
for (const [x, y] of [[0, 0], [1, 1], [2, 0], [4, 6], [9, 9], [8, 2]]) {
    assert(isRed(px(ctx, x, y)), `repeat: (${x},${y}) is red, got ${px(ctx, x, y)}`);
}
for (const [x, y] of [[1, 0], [0, 1], [3, 0], [5, 6], [9, 8], [2, 7]]) {
    assert(isBlue(px(ctx, x, y)), `repeat: (${x},${y}) is blue, got ${px(ctx, x, y)}`);
}

// The pattern is a snapshot: repainting the source afterwards does not
// change what the existing pattern draws.
sctx.fillStyle = 'lime';
sctx.fillRect(0, 0, 2, 2);
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 4, 4)), 'pattern keeps the source as it was at createPattern time');
assert(isBlue(px(ctx, 5, 4)), 'pattern snapshot, off-diagonal still blue');
// Restore the checker for the rest of the test.
sctx.fillStyle = 'red';
sctx.fillRect(0, 0, 1, 1);
sctx.fillRect(1, 1, 1, 1);
sctx.fillStyle = 'blue';
sctx.fillRect(1, 0, 1, 1);
sctx.fillRect(0, 1, 1, 1);

// null and "" both mean repeat.
for (const rep of [null, '']) {
    const p = ctx.createPattern(src, rep);
    ctx.clearRect(0, 0, 10, 10);
    ctx.fillStyle = p;
    ctx.fillRect(0, 0, 10, 10);
    assert(isRed(px(ctx, 6, 8)) && isBlue(px(ctx, 7, 8)), `repetition ${JSON.stringify(rep)} repeats`);
}

// ---------------------------------------------------------------------------
// no-repeat: one tile at the origin, the rest untouched
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = ctx.createPattern(src, 'no-repeat');
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 0, 0)), 'no-repeat: (0,0) red');
assert(isBlue(px(ctx, 1, 0)), 'no-repeat: (1,0) blue');
assert(isRed(px(ctx, 1, 1)), 'no-repeat: (1,1) red');
for (const [x, y] of [[2, 0], [0, 2], [2, 2], [5, 5], [9, 9]]) {
    assert(isClear(px(ctx, x, y)), `no-repeat: (${x},${y}) stays transparent, got ${px(ctx, x, y)}`);
}

// ---------------------------------------------------------------------------
// repeat-x / repeat-y
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = ctx.createPattern(src, 'repeat-x');
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 6, 0)) && isBlue(px(ctx, 7, 0)), 'repeat-x: tiles along x');
assert(isRed(px(ctx, 9, 1)), 'repeat-x: second row tiles too');
assert(isClear(px(ctx, 0, 2)) && isClear(px(ctx, 6, 5)), 'repeat-x: nothing below the first tile row');

ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = ctx.createPattern(src, 'repeat-y');
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 0, 6)) && isBlue(px(ctx, 0, 7)), 'repeat-y: tiles along y');
assert(isClear(px(ctx, 2, 0)) && isClear(px(ctx, 5, 6)), 'repeat-y: nothing right of the first tile column');

// ---------------------------------------------------------------------------
// CanvasPattern.setTransform — applies to later draws with the same object,
// even though it was assigned before the call
// ---------------------------------------------------------------------------
const shifted = ctx.createPattern(src, 'repeat');
ctx.fillStyle = shifted;
shifted.setTransform({ a: 1, b: 0, c: 0, d: 1, e: 1, f: 0 });
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
assert(isBlue(px(ctx, 0, 0)), 'setTransform translate(1,0): (0,0) now blue, got ' + px(ctx, 0, 0));
assert(isRed(px(ctx, 1, 0)), 'setTransform translate(1,0): (1,0) now red');

shifted.setTransform({ m11: 2, m22: 2 });  // scale 2: each texel is 2x2 pixels
// Nearest sampling, so a magnified texel is a solid 2x2 block; with smoothing
// on, bilinear filtering would blend neighbouring texels into every pixel.
ctx.imageSmoothingEnabled = false;
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 0, 0)) && isRed(px(ctx, 1, 1)), 'setTransform scale(2): (0..1,0..1) is the red texel');
assert(isBlue(px(ctx, 2, 0)) && isBlue(px(ctx, 3, 1)), 'setTransform scale(2): (2..3,0..1) is the blue texel');
assert(isRed(px(ctx, 4, 0)), 'setTransform scale(2): the tile repeats at x=4');

// The sampling is read at draw time, not at assignment: the same pattern with
// smoothing back on blends across texel edges when magnified.
ctx.imageSmoothingEnabled = true;
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
{
    const p = px(ctx, 1, 0);
    assert(p[0] > 20 && p[2] > 20, 'smoothing on: a magnified texel edge blends red and blue, got ' + p);
}

shifted.setTransform();  // back to identity
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 0, 0)) && isBlue(px(ctx, 1, 0)), 'setTransform() resets to identity');

let threwBadMatrix = false;
try { shifted.setTransform({ a: NaN }); } catch (e) { threwBadMatrix = e instanceof TypeError; }
assert(threwBadMatrix, 'setTransform with a non-finite member throws TypeError');

// The pattern lives in user space: the current transform moves it.
ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = pat;
ctx.save();
ctx.translate(1, 0);
ctx.fillRect(-1, 0, 11, 10);
ctx.restore();
assert(isBlue(px(ctx, 0, 0)) && isRed(px(ctx, 1, 0)), 'the pattern follows the current transform');

// ---------------------------------------------------------------------------
// globalAlpha modulates a pattern
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = pat;
ctx.globalAlpha = 0.5;
ctx.fillRect(0, 0, 10, 10);
ctx.globalAlpha = 1;
{
    const p = px(ctx, 0, 0);
    assert(near(p[3], 128, 3), 'globalAlpha 0.5 halves the pattern alpha, got ' + p);
}

// ---------------------------------------------------------------------------
// strokeStyle, save/restore, and colors replacing the pattern
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 10, 10);
ctx.strokeStyle = pat;
assert(ctx.strokeStyle === pat, 'strokeStyle answers the pattern object back');
ctx.lineWidth = 2;
ctx.beginPath();
ctx.moveTo(0, 5);
ctx.lineTo(10, 5);
ctx.stroke();
assert(isBlue(px(ctx, 3, 4)) && isRed(px(ctx, 4, 4)), 'stroke draws with the pattern');
assert(isClear(px(ctx, 4, 1)), 'stroke leaves the rest alone');

ctx.fillStyle = pat;
ctx.save();
ctx.fillStyle = '#00ff00';
assert(typeof ctx.fillStyle === 'string', 'a color replaces the pattern');
ctx.restore();
assert(ctx.fillStyle === pat, 'restore brings the pattern back as fillStyle');
ctx.clearRect(0, 0, 10, 10);
ctx.fillRect(0, 0, 10, 10);
assert(isRed(px(ctx, 2, 2)) && isBlue(px(ctx, 3, 2)), 'restored pattern draws');

ctx.fillStyle = 'not a color';
assert(ctx.fillStyle === pat, 'an unparseable color leaves the pattern in place');

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
// ImageBitmap
const greenData = new ImageData(new Uint8ClampedArray([0, 255, 0, 255]), 1, 1);
const bmp = await createImageBitmap(greenData);
ctx.clearRect(0, 0, 10, 10);
ctx.fillStyle = ctx.createPattern(bmp, 'repeat');
ctx.fillRect(0, 0, 10, 10);
{
    const p = px(ctx, 7, 3);
    assert(p[0] < 15 && p[1] > 240 && p[2] < 15 && p[3] > 240, 'ImageBitmap pattern fills green, got ' + p);
}

const closedBmp = await createImageBitmap(greenData);
closedBmp.close();
let closedErr = null;
try { ctx.createPattern(closedBmp, 'repeat'); } catch (e) { closedErr = e; }
assert(closedErr && closedErr.name === 'InvalidStateError', 'a closed ImageBitmap throws InvalidStateError');

// An image that has not loaded yet answers null.
const img = new Image();
assert(ctx.createPattern(img, 'repeat') === null, 'an unloaded image answers null');

// A zero-sized canvas throws InvalidStateError.
const empty = makeCanvas(0, 10);
let emptyErr = null;
try { ctx.createPattern(empty, 'repeat'); } catch (e) { emptyErr = e; }
assert(emptyErr && emptyErr.name === 'InvalidStateError', 'a zero-width canvas throws InvalidStateError');

// ImageData is not a CanvasImageSource.
let typeErr = null;
try { ctx.createPattern(greenData, 'repeat'); } catch (e) { typeErr = e; }
assert(typeErr instanceof TypeError, 'ImageData is not a pattern source (TypeError)');

// ---------------------------------------------------------------------------
// A bad repetition is a SyntaxError
// ---------------------------------------------------------------------------
for (const bad of ['repeat-xy', 'REPEAT', 'no repeat', 'bogus']) {
    let err = null;
    try { ctx.createPattern(src, bad); } catch (e) { err = e; }
    assert(err && err.name === 'SyntaxError', `repetition '${bad}' throws SyntaxError`);
}
