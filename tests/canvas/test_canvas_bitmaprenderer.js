// canvas.getContext('bitmaprenderer'): ImageBitmapRenderingContext and
// transferFromImageBitmap. The bitmaprenderer canvas is read back by drawing
// it onto a 2D canvas and asserting pixels with getImageData.

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

function same(p, q) { return p[0] === q[0] && p[1] === q[1] && p[2] === q[2] && p[3] === q[3]; }

// 2x2: red, green / blue, white.
const quad = new ImageData(new Uint8ClampedArray([
    255, 0, 0, 255,     0, 255, 0, 255,
    0, 0, 255, 255,     255, 255, 255, 255,
]), 2, 2);

const target = makeCanvas(2, 2);
const brc = target.getContext('bitmaprenderer');
assert(brc !== null && typeof brc === 'object', 'getContext("bitmaprenderer") returns a context');
assert(brc instanceof ImageBitmapRenderingContext, 'context is an ImageBitmapRenderingContext');
assert(brc.canvas === target, 'context.canvas is the canvas');
assert(typeof brc.transferFromImageBitmap === 'function', 'transferFromImageBitmap exists');
assert(target.getContext('bitmaprenderer') === brc, 'getContext("bitmaprenderer") is stable');
assert(target.getContext('2d') === null, 'a bitmaprenderer canvas has no 2d context');

const bmp = await createImageBitmap(quad);
assert(bmp.width === 2 && bmp.height === 2, 'bitmap is 2x2');
brc.transferFromImageBitmap(bmp);
assert(bmp.width === 0 && bmp.height === 0, 'the transferred bitmap is detached');

let detachedErr = null;
try { brc.transferFromImageBitmap(bmp); } catch (e) { detachedErr = e; }
assert(detachedErr && detachedErr.name === 'InvalidStateError', 'transferring a detached bitmap throws InvalidStateError');

let typeErr = null;
try { brc.transferFromImageBitmap(quad); } catch (e) { typeErr = e; }
assert(typeErr instanceof TypeError, 'a non-ImageBitmap argument throws TypeError');

// Read the bitmaprenderer canvas back through a 2D canvas.
const reader = makeCanvas(4, 4);
const rctx = reader.getContext('2d');
rctx.drawImage(target, 0, 0);
assert(same(px(rctx, 0, 0), [255, 0, 0, 255]), 'transferred (0,0) red, got ' + px(rctx, 0, 0));
assert(same(px(rctx, 1, 0), [0, 255, 0, 255]), 'transferred (1,0) green, got ' + px(rctx, 1, 0));
assert(same(px(rctx, 0, 1), [0, 0, 255, 255]), 'transferred (0,1) blue, got ' + px(rctx, 0, 1));
assert(same(px(rctx, 1, 1), [255, 255, 255, 255]), 'transferred (1,1) white, got ' + px(rctx, 1, 1));
assert(px(rctx, 2, 0)[3] === 0, 'nothing drawn beyond the 2x2 bitmap');

// A second transfer replaces the output bitmap outright, at the new bitmap's
// size (3x1 magenta), with no trace of the first.
const strip = new ImageData(new Uint8ClampedArray([
    255, 0, 255, 255,   255, 0, 255, 255,   255, 0, 255, 255,
]), 3, 1);
brc.transferFromImageBitmap(await createImageBitmap(strip));
rctx.clearRect(0, 0, 4, 4);
rctx.drawImage(target, 0, 0);
assert(same(px(rctx, 2, 0), [255, 0, 255, 255]), 'second transfer: (2,0) magenta at the 3x1 size, got ' + px(rctx, 2, 0));
assert(px(rctx, 0, 1)[3] === 0, 'second transfer: the old 2x2 bitmap is gone, got ' + px(rctx, 0, 1));

// A bitmap from a 2D canvas (createImageBitmap of an HTMLCanvasElement).
const painted = makeCanvas(2, 2);
const pctx = painted.getContext('2d');
pctx.fillStyle = '#00ff00';
pctx.fillRect(0, 0, 2, 2);
brc.transferFromImageBitmap(await createImageBitmap(painted));
rctx.clearRect(0, 0, 4, 4);
rctx.drawImage(target, 0, 0);
assert(same(px(rctx, 1, 1), [0, 255, 0, 255]), 'canvas-sourced bitmap transfers, got ' + px(rctx, 1, 1));

// null resets the output to transparent black at the canvas's own size.
brc.transferFromImageBitmap(null);
rctx.fillStyle = 'red';
rctx.fillRect(0, 0, 4, 4);
rctx.drawImage(target, 0, 0);
assert(same(px(rctx, 0, 0), [255, 0, 0, 255]), 'after transferFromImageBitmap(null) the canvas draws nothing, got ' + px(rctx, 0, 0));
