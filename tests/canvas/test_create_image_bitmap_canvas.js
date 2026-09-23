// createImageBitmap(<canvas>) for every kind of canvas: a copy of what the
// canvas displays now, at the canvas bitmap's own size — a 2D canvas, a
// bitmaprenderer canvas (the size of the bitmap last transferred in, not the
// width/height attributes), a bitmaprenderer canvas reset with null, and a
// canvas with no context yet (transparent black at its attribute size, and no
// context created by asking).

function makeCanvas(w, h) {
    const c = document.createElement('canvas');
    if (w !== undefined) c.setAttribute('width', String(w));
    if (h !== undefined) c.setAttribute('height', String(h));
    document.body.appendChild(c);
    flush();
    return c;
}

// Read an ImageBitmap's pixels by drawing it onto a fresh 2D canvas.
function bitmapPixels(bmp) {
    const c = makeCanvas(bmp.width, bmp.height);
    const ctx = c.getContext('2d');
    ctx.drawImage(bmp, 0, 0);
    const d = ctx.getImageData(0, 0, bmp.width, bmp.height).data;
    document.body.removeChild(c);
    return d;
}

function pxOf(data, w, x, y) {
    const i = (y * w + x) * 4;
    return [data[i], data[i + 1], data[i + 2], data[i + 3]];
}

function same(p, q) { return p[0] === q[0] && p[1] === q[1] && p[2] === q[2] && p[3] === q[3]; }

function allTransparent(data) {
    for (let i = 3; i < data.length; i += 4) if (data[i] !== 0) return false;
    return true;
}

// 2 wide, 3 tall: red, green / blue, white / black, yellow.
const quad = new ImageData(new Uint8ClampedArray([
    255, 0, 0, 255,     0, 255, 0, 255,
    0, 0, 255, 255,     255, 255, 255, 255,
    0, 0, 0, 255,       255, 255, 0, 255,
]), 2, 3);
const expected = [
    [0, 0, [255, 0, 0, 255]], [1, 0, [0, 255, 0, 255]],
    [0, 1, [0, 0, 255, 255]], [1, 1, [255, 255, 255, 255]],
    [0, 2, [0, 0, 0, 255]],   [1, 2, [255, 255, 0, 255]],
];

// ---------------------------------------------------------------------------
// bitmaprenderer: the transferred bitmap's size, whatever the attributes say
// ---------------------------------------------------------------------------
{
    const c = makeCanvas(8, 8);
    const brc = c.getContext('bitmaprenderer');
    brc.transferFromImageBitmap(await createImageBitmap(quad));

    const bmp = await createImageBitmap(c);
    assert(bmp.width === 2 && bmp.height === 3,
           `bitmaprenderer canvas bitmap is the transferred 2x3, got ${bmp.width}x${bmp.height}`);
    const d = bitmapPixels(bmp);
    for (const [x, y, want] of expected) {
        const got = pxOf(d, 2, x, y);
        assert(same(got, want), `bitmaprenderer pixel ${x},${y} = ${want}, got ${got}`);
    }

    // Cropped.
    const crop = await createImageBitmap(c, 1, 1, 1, 2);
    assert(crop.width === 1 && crop.height === 2, 'cropped bitmaprenderer bitmap is 1x2');
    const cd = bitmapPixels(crop);
    assert(same(pxOf(cd, 1, 0, 0), [255, 255, 255, 255]) && same(pxOf(cd, 1, 0, 1), [255, 255, 0, 255]),
           'cropped bitmaprenderer pixels are the right column, rows 1..2');

    // A second transfer at another size is what the next snapshot sees.
    const one = new ImageData(new Uint8ClampedArray([10, 20, 30, 255, 40, 50, 60, 255,
                                                     70, 80, 90, 255, 1, 2, 3, 255]), 4, 1);
    brc.transferFromImageBitmap(await createImageBitmap(one));
    const bmp2 = await createImageBitmap(c);
    assert(bmp2.width === 4 && bmp2.height === 1, `retransferred 4x1, got ${bmp2.width}x${bmp2.height}`);
    assert(same(pxOf(bitmapPixels(bmp2), 4, 2, 0), [70, 80, 90, 255]), 'retransferred pixels');

    // transferFromImageBitmap(null): transparent black at the canvas's size.
    brc.transferFromImageBitmap(null);
    const cleared = await createImageBitmap(c);
    assert(cleared.width === 8 && cleared.height === 8,
           `null transfer leaves an 8x8 canvas-sized bitmap, got ${cleared.width}x${cleared.height}`);
    assert(allTransparent(bitmapPixels(cleared)), 'null transfer bitmap is transparent');
    document.body.removeChild(c);
}

// bitmaprenderer before any transfer: transparent at the attribute size.
{
    const c = makeCanvas(5, 7);
    c.getContext('bitmaprenderer');
    const bmp = await createImageBitmap(c);
    assert(bmp.width === 5 && bmp.height === 7, `untransferred bitmaprenderer is 5x7, got ${bmp.width}x${bmp.height}`);
    assert(allTransparent(bitmapPixels(bmp)), 'untransferred bitmaprenderer is transparent');
    document.body.removeChild(c);
}

// ---------------------------------------------------------------------------
// No context yet: transparent, canvas-sized, and no context created
// ---------------------------------------------------------------------------
{
    const c = makeCanvas(7, 5);
    const bmp = await createImageBitmap(c);
    assert(bmp.width === 7 && bmp.height === 5, `context-less canvas bitmap is 7x5, got ${bmp.width}x${bmp.height}`);
    assert(allTransparent(bitmapPixels(bmp)), 'context-less canvas bitmap is transparent');
    // Asking did not give the canvas a context of its own: it can still
    // become a bitmaprenderer canvas, and then displays nothing but what it
    // is handed.
    const brc = c.getContext('bitmaprenderer');
    assert(brc !== null, 'context-less canvas can still take any context');
    brc.transferFromImageBitmap(await createImageBitmap(quad));
    const after = await createImageBitmap(c);
    assert(after.width === 2 && after.height === 3, 'and then snapshots at the transferred size');
    document.body.removeChild(c);
}
{
    const c = makeCanvas();   // no attributes: the 300x150 default
    const bmp = await createImageBitmap(c);
    assert(bmp.width === 300 && bmp.height === 150,
           `attribute-less canvas bitmap is 300x150, got ${bmp.width}x${bmp.height}`);
    document.body.removeChild(c);
}

// A zero-sized canvas has no bitmap to copy: InvalidStateError.
{
    const c = makeCanvas(0, 10);
    let err = null;
    try { await createImageBitmap(c); } catch (e) { err = e; }
    assert(err && err.name === 'InvalidStateError', 'zero-width canvas rejects with InvalidStateError, got ' + err);
    document.body.removeChild(c);
}

// ---------------------------------------------------------------------------
// 2D: the current bitmap size, following canvas.width
// ---------------------------------------------------------------------------
{
    const c = makeCanvas(6, 6);
    const ctx = c.getContext('2d');
    ctx.fillStyle = 'rgb(0, 128, 255)';
    ctx.fillRect(0, 0, 6, 6);
    const bmp = await createImageBitmap(c);
    assert(bmp.width === 6 && bmp.height === 6, '2D canvas bitmap 6x6');
    assert(same(pxOf(bitmapPixels(bmp), 6, 5, 5), [0, 128, 255, 255]), '2D canvas pixels');

    c.width = 9;
    c.height = 4;
    ctx.fillStyle = 'rgb(255, 0, 128)';
    ctx.fillRect(8, 3, 1, 1);
    const bmp2 = await createImageBitmap(c);
    assert(bmp2.width === 9 && bmp2.height === 4, `resized 2D canvas bitmap 9x4, got ${bmp2.width}x${bmp2.height}`);
    const d2 = bitmapPixels(bmp2);
    assert(same(pxOf(d2, 9, 8, 3), [255, 0, 128, 255]), 'resized 2D canvas corner pixel');
    assert(pxOf(d2, 9, 0, 0)[3] === 0, 'resize cleared the old content');
    document.body.removeChild(c);
}
