// ctx.filter: the CSS filter functions applied to subsequent draws, the
// "none" default, invalid values ignored, and the state stack. Asserted on
// pixels read back with getImageData.

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

const cv = makeCanvas(20, 20);
const ctx = cv.getContext('2d');

// Fill the whole canvas with `color` under `filter`, then read the centre.
function filtered(filter, color, extra) {
    ctx.clearRect(0, 0, 20, 20);
    ctx.filter = filter;
    if (extra) extra();
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, 20, 20);
    ctx.filter = 'none';
    ctx.globalAlpha = 1;
    return px(ctx, 10, 10);
}

// ---------------------------------------------------------------------------
// The attribute
// ---------------------------------------------------------------------------
assert(ctx.filter === 'none', 'filter defaults to "none", got ' + ctx.filter);
ctx.filter = 'grayscale(1)';
assert(ctx.filter === 'grayscale(1)', 'filter answers the value set');
for (const bad of ['bogus(3)', 'blur(-1px)', 'blur(5)', 'grayscale(-1)', 'hue-rotate(90)',
                   'grayscale(1', '', 'url(#f)', 'blur(1px) nope']) {
    ctx.filter = bad;
    assert(ctx.filter === 'grayscale(1)', `invalid filter '${bad}' is ignored, got ${ctx.filter}`);
}
ctx.save();
ctx.filter = 'blur(2px)';
assert(ctx.filter === 'blur(2px)', 'filter set inside save');
ctx.restore();
assert(ctx.filter === 'grayscale(1)', 'restore brings the filter back');
ctx.filter = 'none';
assert(ctx.filter === 'none', 'filter = "none" clears it');

// ---------------------------------------------------------------------------
// Color functions
// ---------------------------------------------------------------------------
{
    // grayscale(1): red becomes the gray of its luma (0.2126 * 255 = 54).
    const p = filtered('grayscale(1)', 'red');
    assert(p[0] === p[1] && p[1] === p[2], 'grayscale: r == g == b, got ' + p);
    assert(near(p[0], 54, 4) && p[3] === 255, 'grayscale(1) red -> ~54 gray, got ' + p);
}
{
    const p = filtered('grayscale(100%)', 'red');
    assert(near(p[0], 54, 4) && p[0] === p[2], 'grayscale(100%) is grayscale(1), got ' + p);
}
{
    const p = filtered('saturate(0)', 'red');
    assert(p[0] === p[1] && p[1] === p[2] && near(p[0], 54, 4), 'saturate(0) desaturates, got ' + p);
}
{
    const p = filtered('invert(1)', 'red');
    assert(p[0] < 5 && p[1] > 250 && p[2] > 250, 'invert(1) red -> cyan, got ' + p);
}
{
    const p = filtered('invert(0)', 'red');
    assert(p[0] > 250 && p[1] < 5 && p[2] < 5, 'invert(0) is a no-op, got ' + p);
}
{
    const p = filtered('opacity(0.5)', 'red');
    assert(p[0] > 240 && near(p[3], 128, 3), 'opacity(0.5) halves alpha, got ' + p);
}
{
    const p = filtered('brightness(0.5)', 'white');
    assert(near(p[0], 128, 4) && near(p[1], 128, 4) && p[3] === 255, 'brightness(0.5) white -> ~128, got ' + p);
}
{
    const p = filtered('contrast(0)', 'red');
    assert(near(p[0], 128, 4) && near(p[1], 128, 4) && near(p[2], 128, 4), 'contrast(0) -> mid gray, got ' + p);
}
{
    const p = filtered('sepia(1)', 'white');
    assert(p[0] === 255 && p[1] === 255 && near(p[2], 239, 4), 'sepia(1) white -> (255,255,~239), got ' + p);
}
{
    // hue-rotate(180deg) of pure red, by the Filter Effects matrix: (0, 109, 109).
    const p = filtered('hue-rotate(180deg)', 'red');
    assert(p[0] < 8 && near(p[1], 109, 5) && near(p[2], 109, 5), 'hue-rotate(180deg) red, got ' + p);
    const q = filtered('hue-rotate(0.5turn)', 'red');
    assert(near(q[1], p[1], 2) && near(q[2], p[2], 2), 'hue-rotate(0.5turn) == hue-rotate(180deg), got ' + q);
}
{
    // A chain applies left to right.
    const p = filtered('invert(1) opacity(0.5)', 'red');
    assert(p[0] < 5 && p[1] > 245 && near(p[3], 128, 3), 'invert(1) opacity(0.5), got ' + p);
}
{
    // globalAlpha still applies, after the filter.
    const p = filtered('grayscale(1)', 'red', () => { ctx.globalAlpha = 0.5; });
    assert(p[0] === p[1] && p[1] === p[2] && near(p[3], 128, 3), 'grayscale + globalAlpha 0.5, got ' + p);
}
{
    // Unfiltered after "none".
    const p = filtered('none', 'red');
    assert(p[0] === 255 && p[1] === 0 && p[3] === 255, 'filter none draws red unchanged, got ' + p);
}

// ---------------------------------------------------------------------------
// blur(): ink spreads into a neighbouring pixel
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 20, 20);
ctx.fillStyle = 'black';
ctx.fillRect(10, 0, 10, 20);
assert(px(ctx, 9, 10)[3] === 0, 'control: no blur, the pixel left of the edge is empty');

ctx.clearRect(0, 0, 20, 20);
ctx.filter = 'blur(2px)';
ctx.fillRect(10, 0, 10, 20);
ctx.filter = 'none';
{
    const left = px(ctx, 9, 10)[3];
    const far = px(ctx, 2, 10)[3];
    const inner = px(ctx, 10, 10)[3];
    // Mid-rect: 5px from its left edge and from the canvas's right edge,
    // beyond which the blur samples transparency.
    const deep = px(ctx, 15, 10)[3];
    assert(left > 40, 'blur spreads ink into the pixel left of the edge, alpha ' + left);
    assert(inner < 255 && inner > left, 'blur softens the edge pixel itself, alpha ' + inner);
    assert(far < 5, 'blur(2px) does not reach 8px away, alpha ' + far);
    assert(deep > 240, 'deep inside the rect stays solid, alpha ' + deep);
}

// The blur radius is in canvas pixels, not scaled by the transform.
ctx.clearRect(0, 0, 20, 20);
ctx.save();
ctx.scale(4, 4);
ctx.filter = 'blur(1px)';
ctx.fillRect(2.5, 0, 5, 5);   // device x 10..20
ctx.restore();
{
    const near1 = px(ctx, 9, 10)[3];
    const far = px(ctx, 5, 10)[3];
    assert(near1 > 20, 'scaled blur still spreads one pixel, alpha ' + near1);
    assert(far < 5, 'blur(1px) under scale(4) is not a 4px blur, alpha at 5px ' + far);
}

// ---------------------------------------------------------------------------
// drop-shadow(): an offset copy behind the drawing
// ---------------------------------------------------------------------------
ctx.clearRect(0, 0, 20, 20);
ctx.filter = 'drop-shadow(4px 0px 0px blue)';
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 6, 6);
ctx.filter = 'none';
{
    const body = px(ctx, 2, 2);
    const shadow = px(ctx, 8, 2);
    const outside = px(ctx, 12, 2);
    assert(body[0] > 240 && body[2] < 15, 'drop-shadow keeps the drawing on top, got ' + body);
    assert(shadow[2] > 240 && shadow[0] < 15 && shadow[3] > 240, 'drop-shadow paints the offset copy blue, got ' + shadow);
    assert(outside[3] === 0, 'drop-shadow stops past the offset copy, got ' + outside);
}
// The color may come first, and in functional notation.
ctx.filter = 'drop-shadow(rgba(0, 0, 255, 1) 4px 0px)';
assert(ctx.filter === 'drop-shadow(rgba(0, 0, 255, 1) 4px 0px)', 'drop-shadow with a leading functional color parses');

// A drop-shadow() with no colour, or `currentcolor`, is the canvas element's
// `color` at the time of the assignment (it was black). Changing `color`
// afterwards does not recolour the filter already set.
function shadowOf(filter) {
    ctx.clearRect(0, 0, 20, 20);
    ctx.filter = filter;
    ctx.fillStyle = 'red';
    ctx.fillRect(0, 0, 6, 6);
    ctx.filter = 'none';
    return px(ctx, 8, 2);
}
cv.style.color = 'rgb(0, 200, 0)';
for (const f of ['drop-shadow(4px 0px 0px)', 'drop-shadow(currentcolor 4px 0px)',
                 'drop-shadow(4px 0px currentColor)']) {
    const s = shadowOf(f);
    assert(s[0] < 15 && near(s[1], 200, 3) && s[2] < 15 && s[3] > 240,
           f + ' paints the canvas colour rgb(0, 200, 0), got ' + s);
}
{
    ctx.clearRect(0, 0, 20, 20);
    ctx.filter = 'drop-shadow(4px 0px 0px)';
    cv.style.color = 'rgb(0, 0, 255)';
    ctx.fillStyle = 'red';
    ctx.fillRect(0, 0, 6, 6);
    ctx.filter = 'none';
    const s = px(ctx, 8, 2);
    assert(near(s[1], 200, 3) && s[2] < 15,
           'the colour is fixed when the filter is set, got ' + s);
    cv.style.color = '';
}

// ---------------------------------------------------------------------------
// The filter reaches every kind of draw
// ---------------------------------------------------------------------------
{
    // stroke
    ctx.clearRect(0, 0, 20, 20);
    ctx.filter = 'grayscale(1)';
    ctx.strokeStyle = 'red';
    ctx.lineWidth = 4;
    ctx.beginPath();
    ctx.moveTo(0, 10);
    ctx.lineTo(20, 10);
    ctx.stroke();
    ctx.filter = 'none';
    const p = px(ctx, 10, 10);
    assert(p[0] === p[1] && near(p[0], 54, 4), 'grayscale applies to stroke, got ' + p);
}
{
    // drawImage
    const src = makeCanvas(4, 4);
    const sctx = src.getContext('2d');
    sctx.fillStyle = 'red';
    sctx.fillRect(0, 0, 4, 4);
    ctx.clearRect(0, 0, 20, 20);
    ctx.filter = 'invert(1)';
    ctx.drawImage(src, 0, 0, 20, 20);
    ctx.filter = 'none';
    const p = px(ctx, 10, 10);
    assert(p[0] < 5 && p[1] > 250 && p[2] > 250, 'invert applies to drawImage, got ' + p);
}
{
    // clearRect is not filtered: it still clears
    ctx.fillStyle = 'red';
    ctx.fillRect(0, 0, 20, 20);
    ctx.filter = 'blur(3px)';
    ctx.clearRect(5, 5, 10, 10);
    ctx.filter = 'none';
    assert(px(ctx, 10, 10)[3] === 0, 'clearRect ignores the filter');
    assert(px(ctx, 4, 10)[3] === 255, 'clearRect with a filter does not blur its edge');
}
