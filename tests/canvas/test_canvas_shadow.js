// Canvas shadows: shadowColor / shadowBlur / shadowOffsetX / shadowOffsetY on
// every drawing operation, with globalAlpha, the composite operation, the clip,
// the transform (which the offsets ignore) and ctx.filter. Asserted on pixels
// read back with getImageData.

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
function isColor(p, r, g, b, a, tol) {
    tol = tol === undefined ? 3 : tol;
    return near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol) && near(p[3], a, tol);
}
function isClear(p) { return p[3] === 0; }

const W = 60, H = 60;
const cv = makeCanvas(W, H);
const ctx = cv.getContext('2d');

// Clear everything and put the drawing state back to a known shadow setup.
function fresh(color, ox, oy, blur) {
    ctx.reset();
    ctx.shadowColor = color;
    ctx.shadowOffsetX = ox;
    ctx.shadowOffsetY = oy;
    ctx.shadowBlur = blur;
}

// ---------------------------------------------------------------------------
// The attributes
// ---------------------------------------------------------------------------
ctx.reset();
assert(ctx.shadowBlur === 0, 'shadowBlur defaults to 0');
assert(ctx.shadowOffsetX === 0 && ctx.shadowOffsetY === 0, 'shadow offsets default to 0');
{
    // The default colour is transparent black, however it is serialized.
    const probe = makeCanvas(1, 1).getContext('2d');
    assert(ctx.shadowColor === probe.shadowColor, 'shadowColor default matches a fresh context');
}
ctx.shadowBlur = 4;
for (const bad of [-1, NaN, Infinity, -Infinity]) {
    ctx.shadowBlur = bad;
    assert(ctx.shadowBlur === 4, `shadowBlur = ${bad} is ignored, got ${ctx.shadowBlur}`);
}
ctx.shadowOffsetX = 3;
ctx.shadowOffsetY = -2;
for (const bad of [NaN, Infinity, -Infinity]) {
    ctx.shadowOffsetX = bad;
    ctx.shadowOffsetY = bad;
    assert(ctx.shadowOffsetX === 3 && ctx.shadowOffsetY === -2, `shadow offset = ${bad} is ignored`);
}
ctx.save();
ctx.shadowOffsetX = 9;
ctx.shadowColor = 'red';
ctx.restore();
assert(ctx.shadowOffsetX === 3, 'restore brings shadowOffsetX back');

// ---------------------------------------------------------------------------
// fillRect, offset only: a hard-edged copy of the shape in the shadow colour
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(isColor(px(ctx, 10, 10), 255, 0, 0, 255), 'shape drawn over its shadow, got ' + px(ctx, 10, 10));
assert(isColor(px(ctx, 20, 20), 0, 0, 255, 255), 'offset shadow is blue, got ' + px(ctx, 20, 20));
assert(isColor(px(ctx, 24, 24), 0, 0, 255, 255), 'shadow covers the offset rect, got ' + px(ctx, 24, 24));
assert(isClear(px(ctx, 27, 27)), 'nothing past the shadow, got ' + px(ctx, 27, 27));
assert(isClear(px(ctx, 22, 8)), 'nothing beside the shadow, got ' + px(ctx, 22, 8));

// Negative offsets go the other way.
fresh('blue', -10, 0, 0);
ctx.fillStyle = 'red';
ctx.fillRect(30, 30, 10, 10);
assert(isColor(px(ctx, 22, 35), 0, 0, 255, 255), 'negative offsetX shadow to the left, got ' + px(ctx, 22, 35));
assert(isClear(px(ctx, 45, 35)), 'no shadow to the right');

// ---------------------------------------------------------------------------
// When there is no shadow
// ---------------------------------------------------------------------------
fresh('transparent', 10, 10, 5);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(isClear(px(ctx, 20, 20)), 'a transparent shadowColor draws no shadow');

fresh('rgba(0,0,255,0)', 10, 10, 5);
ctx.fillRect(5, 5, 10, 10);
assert(isClear(px(ctx, 20, 20)), 'a zero-alpha shadowColor draws no shadow');

fresh('blue', 0, 0, 0);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(isClear(px(ctx, 16, 10)), 'zero blur and zero offsets draw no shadow');
assert(isColor(px(ctx, 10, 10), 255, 0, 0, 255), 'the shape still draws');

// The default state has no shadow at all.
ctx.reset();
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(isClear(px(ctx, 16, 16)), 'default state draws no shadow');

// ---------------------------------------------------------------------------
// Blur: sigma = shadowBlur / 2, so shadowBlur 8 fades out over ~12px
// ---------------------------------------------------------------------------
fresh('blue', 0, 0, 8);
ctx.fillStyle = 'red';
ctx.fillRect(20, 20, 20, 20);
{
    const edge = px(ctx, 18, 30);   // 2px outside the shape: a soft shadow
    assert(edge[3] > 40 && edge[3] < 220 && edge[2] > 200 && edge[0] < 20,
           'blurred shadow is partial blue just outside the shape, got ' + edge);
    const mid = px(ctx, 14, 30);    // 6px out: fainter
    assert(mid[3] > 0 && mid[3] < edge[3], 'shadow fades with distance, got ' + mid + ' vs ' + edge);
    const far = px(ctx, 3, 30);     // 17px out, past 4 sigma
    assert(far[3] <= 2, 'blurred shadow gone far from the shape, got ' + far);
    assert(isColor(px(ctx, 30, 30), 255, 0, 0, 255), 'shape itself unblurred');
}

// A larger blur spreads further.
fresh('blue', 0, 0, 20);
ctx.fillStyle = 'red';
ctx.fillRect(20, 20, 20, 20);
assert(px(ctx, 10, 30)[3] > 10, 'shadowBlur 20 still visible 10px out, got ' + px(ctx, 10, 30));

// ---------------------------------------------------------------------------
// The transform moves the shape but never the offset
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
ctx.scale(2, 2);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 5, 5);   // device 10..20
assert(isColor(px(ctx, 15, 15), 255, 0, 0, 255), 'scaled shape at device 10..20');
assert(isColor(px(ctx, 25, 25), 0, 0, 255, 255), 'shadow offset 10 device px, unscaled, got ' + px(ctx, 25, 25));
assert(isClear(px(ctx, 35, 35)), 'the offset is not scaled to 20, got ' + px(ctx, 35, 35));

fresh('blue', 10, 0, 0);
ctx.translate(40, 10);
ctx.rotate(Math.PI / 2);       // local +x is device +y
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 5, 5);      // device x 35..40, y 10..15
assert(isColor(px(ctx, 37, 12), 255, 0, 0, 255), 'rotated shape, got ' + px(ctx, 37, 12));
assert(isColor(px(ctx, 47, 12), 0, 0, 255, 255), 'shadow still offset along device +x, got ' + px(ctx, 47, 12));
assert(isClear(px(ctx, 37, 22)), 'shadow not offset along the rotated axis, got ' + px(ctx, 37, 22));

// ---------------------------------------------------------------------------
// Alpha: the shadow is cast from the shape's alpha, scaled by the shadow
// colour's alpha and by globalAlpha
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
ctx.globalAlpha = 0.5;
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
{
    const s = px(ctx, 22, 22);
    assert(near(s[3], 128, 4) && s[2] > 250 && s[0] < 5, 'globalAlpha halves the shadow, got ' + s);
    const f = px(ctx, 8, 8);
    assert(near(f[3], 128, 4) && f[0] > 250, 'and the shape, got ' + f);
}

fresh('rgba(0,0,255,0.5)', 10, 10, 0);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(near(px(ctx, 22, 22)[3], 128, 4), 'shadowColor alpha scales the shadow, got ' + px(ctx, 22, 22));

fresh('blue', 10, 10, 0);
ctx.fillStyle = 'rgba(255,0,0,0.5)';
ctx.fillRect(5, 5, 10, 10);
assert(near(px(ctx, 22, 22)[3], 128, 4), 'a half-transparent shape casts a half shadow, got ' + px(ctx, 22, 22));

// Where the shape overlaps its own shadow, the shape composites over it: a
// half-transparent red over its half-transparent blue shadow is 0.75 alpha,
// two parts red to one part blue.
fresh('blue', 5, 5, 0);
ctx.fillStyle = 'rgba(255,0,0,0.5)';
ctx.fillRect(5, 5, 20, 20);
{
    const p = px(ctx, 15, 15);   // shape over shadow
    assert(isColor(p, 170, 0, 85, 191, 4), 'shape composited over its own shadow, got ' + p);
}

// ---------------------------------------------------------------------------
// The composite operation applies to the shadow and to the shape separately
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
ctx.shadowColor = 'transparent';
ctx.fillStyle = 'lime';
ctx.fillRect(18, 18, 10, 10);            // destination content, no shadow
ctx.shadowColor = 'blue';
ctx.globalCompositeOperation = 'destination-over';
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);             // shadow lands at 15..25
assert(isColor(px(ctx, 20, 20), 0, 255, 0, 255), 'destination-over keeps the lime in front of the shadow, got ' + px(ctx, 20, 20));
assert(isColor(px(ctx, 16, 16), 0, 0, 255, 255), 'destination-over shadow fills where there was nothing, got ' + px(ctx, 16, 16));
assert(isColor(px(ctx, 8, 8), 255, 0, 0, 255), 'and the shape too, got ' + px(ctx, 8, 8));

fresh('blue', 10, 10, 0);
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);           // red 5..15, blue shadow 15..25
ctx.globalCompositeOperation = 'destination-out';
ctx.shadowColor = 'black';
ctx.shadowOffsetX = 12;
ctx.shadowOffsetY = 12;
ctx.fillRect(6, 6, 4, 4);             // shape 6..10, shadow 18..22
ctx.globalCompositeOperation = 'source-over';
assert(isClear(px(ctx, 20, 20)), 'destination-out shadow erases, got ' + px(ctx, 20, 20));
assert(isClear(px(ctx, 8, 8)), 'destination-out shape erases too, got ' + px(ctx, 8, 8));
assert(isColor(px(ctx, 24, 24), 0, 0, 255, 255), 'blue outside the erasing shadow kept, got ' + px(ctx, 24, 24));
assert(isColor(px(ctx, 12, 12), 255, 0, 0, 255), 'red outside the erasing shape kept, got ' + px(ctx, 12, 12));

// ---------------------------------------------------------------------------
// The clip bounds the shadow, not the shape it is cast from
// ---------------------------------------------------------------------------
fresh('blue', 20, 20, 0);
ctx.beginPath();
ctx.rect(15, 15, 20, 20);
ctx.clip();
ctx.fillStyle = 'red';
ctx.fillRect(0, 0, 10, 10);    // outside the clip; its shadow (20..30) inside
assert(isClear(px(ctx, 5, 5)), 'the shape is clipped away');
assert(isColor(px(ctx, 25, 25), 0, 0, 255, 255), 'the shadow of a clipped-out shape still lands inside the clip, got ' + px(ctx, 25, 25));
ctx.fillRect(10, 10, 20, 20);  // shadow 30..50, clip ends at 35
assert(isColor(px(ctx, 33, 33), 0, 0, 255, 255), 'shadow inside the clip, got ' + px(ctx, 33, 33));
assert(isClear(px(ctx, 40, 40)), 'shadow cut at the clip edge, got ' + px(ctx, 40, 40));

// ---------------------------------------------------------------------------
// Every drawing operation casts one
// ---------------------------------------------------------------------------
// strokeRect
fresh('blue', 10, 10, 0);
ctx.strokeStyle = 'red';
ctx.lineWidth = 4;
ctx.strokeRect(10, 10, 20, 20);
// Stroke band 8..12 on each edge of 10..30; its shadow the same, 10px on.
assert(isColor(px(ctx, 20, 10), 255, 0, 0, 255), 'strokeRect draws its stroke, got ' + px(ctx, 20, 10));
assert(isColor(px(ctx, 38, 20), 0, 0, 255, 255), 'strokeRect shadow of the top edge, got ' + px(ctx, 38, 20));
assert(isClear(px(ctx, 25, 25)), 'the stroke shadow is hollow, got ' + px(ctx, 25, 25));

// fill() of a path
fresh('blue', 0, 15, 0);
ctx.fillStyle = 'red';
ctx.beginPath();
ctx.arc(20, 15, 8, 0, Math.PI * 2);
ctx.fill();
assert(isColor(px(ctx, 20, 15), 255, 0, 0, 255), 'arc filled');
assert(isColor(px(ctx, 20, 30), 0, 0, 255, 255), 'fill() shadow, got ' + px(ctx, 20, 30));

// fill(Path2D)
fresh('blue', 15, 0, 0);
ctx.fillStyle = 'red';
const p2d = new Path2D();
p2d.rect(5, 40, 10, 10);
ctx.fill(p2d);
assert(isColor(px(ctx, 25, 45), 0, 0, 255, 255), 'fill(Path2D) shadow, got ' + px(ctx, 25, 45));

// stroke() of a path
fresh('blue', 0, 10, 0);
ctx.strokeStyle = 'red';
ctx.lineWidth = 6;
ctx.beginPath();
ctx.moveTo(5, 10);
ctx.lineTo(50, 10);
ctx.stroke();
assert(isColor(px(ctx, 30, 20), 0, 0, 255, 255), 'stroke() shadow, got ' + px(ctx, 30, 20));

// A gradient fill casts a shadow of its alpha.
fresh('blue', 10, 10, 0);
const grad = ctx.createLinearGradient(5, 0, 15, 0);
grad.addColorStop(0, 'red');
grad.addColorStop(1, 'yellow');
ctx.fillStyle = grad;
ctx.fillRect(5, 5, 10, 10);
assert(isColor(px(ctx, 20, 20), 0, 0, 255, 255), 'gradient fill shadow, got ' + px(ctx, 20, 20));

// fillText / strokeText: some shadow-coloured ink below the text.
function inkCount(x, y, w, h, pred) {
    const d = ctx.getImageData(x, y, w, h).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4) if (pred(d[i], d[i + 1], d[i + 2], d[i + 3])) n++;
    return n;
}
const blueInk = (r, g, b, a) => a > 128 && b > 200 && r < 60;
const redInk = (r, g, b, a) => a > 128 && r > 200 && b < 60;
fresh('blue', 0, 30, 0);
ctx.font = '24px sans-serif';
ctx.fillStyle = 'red';
ctx.fillText('WW', 5, 24);
assert(inkCount(0, 0, W, 28, redInk) > 20, 'fillText draws red text');
assert(inkCount(0, 30, W, 30, blueInk) > 20, 'fillText casts a blue shadow 30px below, got ' +
       inkCount(0, 30, W, 30, blueInk));
assert(inkCount(0, 0, W, 28, blueInk) === 0, 'no text shadow above the offset');

fresh('blue', 0, 30, 0);
ctx.font = '24px sans-serif';
ctx.strokeStyle = 'red';
ctx.lineWidth = 2;
ctx.strokeText('WW', 5, 24);
assert(inkCount(0, 30, W, 30, blueInk) > 5, 'strokeText casts a shadow');

// drawImage: the shadow follows the image's alpha.
const src = makeCanvas(10, 10);
const sctx = src.getContext('2d');
sctx.fillStyle = 'red';
sctx.fillRect(0, 0, 10, 5);      // only the top half opaque
fresh('blue', 10, 10, 0);
ctx.drawImage(src, 5, 5);
assert(isColor(px(ctx, 8, 7), 255, 0, 0, 255), 'drawImage draws the image');
assert(isColor(px(ctx, 18, 17), 0, 0, 255, 255), 'drawImage shadow from the opaque half, got ' + px(ctx, 18, 17));
assert(isClear(px(ctx, 18, 23)), 'no shadow from the transparent half, got ' + px(ctx, 18, 23));

// drawImage of an ImageBitmap, scaled.
const bmp = await createImageBitmap(src);
fresh('blue', 0, 25, 0);
ctx.drawImage(bmp, 30, 5, 20, 20);
assert(isColor(px(ctx, 40, 32), 0, 0, 255, 255), 'drawImage(ImageBitmap) shadow, got ' + px(ctx, 40, 32));

// ---------------------------------------------------------------------------
// Operations that never cast one
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
const patch = ctx.createImageData(10, 10);
for (let i = 0; i < patch.data.length; i += 4) { patch.data[i] = 255; patch.data[i + 3] = 255; }
ctx.putImageData(patch, 5, 5);
assert(isColor(px(ctx, 8, 8), 255, 0, 0, 255), 'putImageData writes its pixels');
assert(isClear(px(ctx, 20, 20)), 'putImageData casts no shadow, got ' + px(ctx, 20, 20));

ctx.reset();
ctx.fillStyle = 'lime';
ctx.fillRect(0, 0, W, H);
ctx.shadowColor = 'blue';
ctx.shadowOffsetX = 10;
ctx.shadowOffsetY = 10;
ctx.clearRect(5, 5, 10, 10);
assert(isClear(px(ctx, 8, 8)), 'clearRect clears');
assert(isColor(px(ctx, 20, 20), 0, 255, 0, 255), 'clearRect casts no shadow, got ' + px(ctx, 20, 20));

// ---------------------------------------------------------------------------
// With ctx.filter: the shadow is cast from the filtered image
// ---------------------------------------------------------------------------
fresh('blue', 10, 10, 0);
ctx.filter = 'opacity(0.5)';
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
{
    const s = px(ctx, 22, 22);
    assert(near(s[3], 128, 5) && s[2] > 250, 'shadow of an opacity(0.5)-filtered shape is half, got ' + s);
    const f = px(ctx, 8, 8);
    assert(near(f[3], 128, 5) && f[0] > 250, 'the filtered shape is half too, got ' + f);
}

fresh('blue', 10, 10, 0);
ctx.filter = 'grayscale(1)';
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
{
    const f = px(ctx, 8, 8);
    assert(near(f[0], f[1], 3) && near(f[1], f[2], 3), 'filter still applies to the shape, got ' + f);
    assert(isColor(px(ctx, 22, 22), 0, 0, 255, 255), 'the shadow keeps its own colour, got ' + px(ctx, 22, 22));
}

// A filter blur widens the shape, and the shadow is cast from the widened
// shape: there is shadow just past where an unfiltered shadow would end.
fresh('blue', 20, 0, 0);
ctx.filter = 'blur(3px)';
ctx.fillStyle = 'red';
ctx.fillRect(10, 20, 10, 10);    // shadow 30..40
assert(px(ctx, 41, 25)[3] > 10 && px(ctx, 41, 25)[2] > 150, 'shadow of the blurred shape is blurred, got ' + px(ctx, 41, 25));

// With globalAlpha and a filter both set.
fresh('blue', 10, 10, 0);
ctx.filter = 'grayscale(1)';
ctx.globalAlpha = 0.5;
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(near(px(ctx, 22, 22)[3], 128, 5), 'filter + globalAlpha shadow is half, got ' + px(ctx, 22, 22));

// ---------------------------------------------------------------------------
// Shadow state rides the state stack
// ---------------------------------------------------------------------------
fresh('transparent', 0, 0, 0);
ctx.save();
ctx.shadowColor = 'blue';
ctx.shadowOffsetX = 10;
ctx.shadowOffsetY = 10;
ctx.restore();
ctx.fillStyle = 'red';
ctx.fillRect(5, 5, 10, 10);
assert(isClear(px(ctx, 20, 20)), 'restore() drops the shadow set after save()');

document.body.removeChild(cv);
document.body.removeChild(src);
