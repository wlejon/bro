// ctx.reset() and canvas.width/height assignment: both clear the bitmap AND
// put the whole drawing state back to its defaults — the state stack, the
// current path, the transform, the clip, the line dash and every attribute.

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

const W = 40, H = 40;

// The attributes a reset must restore, read off a context nothing has touched
// so the expectation is whatever this binding reports as the default.
const ATTRS = [
    'fillStyle', 'strokeStyle', 'lineWidth', 'lineCap', 'lineJoin', 'miterLimit',
    'lineDashOffset', 'globalAlpha', 'globalCompositeOperation', 'font', 'textAlign',
    'textBaseline', 'direction', 'shadowBlur', 'shadowColor', 'shadowOffsetX',
    'shadowOffsetY', 'imageSmoothingEnabled', 'imageSmoothingQuality', 'filter',
];
const pristine = makeCanvas(W, H).getContext('2d');
const defaults = {};
for (const k of ATTRS) defaults[k] = pristine[k];

// Dirty every piece of state there is, with a save() on the stack, a clip, a
// transform and a half-built path.
function dirty(ctx) {
    ctx.fillStyle = 'red';
    ctx.fillRect(0, 0, W, H);
    const g = ctx.createLinearGradient(0, 0, 10, 0);
    g.addColorStop(0, 'red');
    g.addColorStop(1, 'blue');
    ctx.save();
    ctx.fillStyle = g;
    ctx.strokeStyle = 'blue';
    ctx.lineWidth = 7;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'bevel';
    ctx.miterLimit = 3;
    ctx.setLineDash([4, 2]);
    ctx.lineDashOffset = 1.5;
    ctx.globalAlpha = 0.25;
    ctx.globalCompositeOperation = 'xor';
    ctx.font = 'bold 30px serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.direction = 'rtl';
    ctx.shadowBlur = 3;
    ctx.shadowColor = 'lime';
    ctx.shadowOffsetX = 4;
    ctx.shadowOffsetY = 5;
    ctx.imageSmoothingEnabled = false;
    ctx.imageSmoothingQuality = 'high';
    ctx.filter = 'blur(2px)';
    ctx.translate(10, 10);
    ctx.scale(2, 2);
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, 2, 2);
    ctx.clip();
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(W, 0);
    ctx.lineTo(W, H);   // an open path a later fill() would close and fill
}

function checkDefaults(ctx, how) {
    for (const k of ATTRS) {
        assert(ctx[k] === defaults[k], `${how}: ${k} back to ${defaults[k]}, got ${ctx[k]}`);
    }
    assert(typeof ctx.fillStyle === 'string', `${how}: a gradient fillStyle is gone`);
    assert(ctx.getLineDash().length === 0, `${how}: line dash emptied, got [${ctx.getLineDash()}]`);
    const m = ctx.getTransform();
    assert(m.a === 1 && m.b === 0 && m.c === 0 && m.d === 1 && m.e === 0 && m.f === 0,
           `${how}: transform is the identity, got ${[m.a, m.b, m.c, m.d, m.e, m.f]}`);

    // The bitmap is transparent black.
    assert(px(ctx, 0, 0)[3] === 0 && px(ctx, W - 1, H - 1)[3] === 0, `${how}: bitmap cleared`);

    // The current path is empty: fill() draws nothing.
    ctx.fill();
    assert(px(ctx, W - 5, 10)[3] === 0, `${how}: the old path is gone, got ${px(ctx, W - 5, 10)}`);

    // The state stack is empty: restore() is a no-op, and the clip and the
    // transform are gone, so a full-canvas fill covers every corner in the
    // default opaque black.
    ctx.restore();
    ctx.restore();
    ctx.fillRect(0, 0, W, H);
    for (const [x, y] of [[0, 0], [W - 1, 0], [0, H - 1], [W - 1, H - 1], [20, 20]]) {
        const p = px(ctx, x, y);
        assert(p[0] === 0 && p[1] === 0 && p[2] === 0 && p[3] === 255,
               `${how}: unclipped, untransformed, unshadowed black at ${x},${y}, got ${p}`);
    }
    // Attributes still default after the restore() calls.
    assert(ctx.globalAlpha === defaults.globalAlpha, `${how}: restore() after reset brings nothing back`);
}

// ---------------------------------------------------------------------------
// ctx.reset()
// ---------------------------------------------------------------------------
{
    const c = makeCanvas(W, H);
    const ctx = c.getContext('2d');
    dirty(ctx);
    ctx.reset();
    checkDefaults(ctx, 'reset()');

    // Drawing state set right after a reset sticks.
    ctx.reset();
    ctx.fillStyle = 'blue';
    ctx.translate(5, 0);
    ctx.fillRect(0, 0, 5, 5);
    const p = px(ctx, 7, 2);
    assert(p[2] === 255 && p[3] === 255, 'state set after reset() applies, got ' + p);
    assert(px(ctx, 2, 2)[3] === 0, 'the new transform applies too');
    document.body.removeChild(c);
}

// Everything recorded before a reset in the same turn is gone, however much.
{
    const c = makeCanvas(W, H);
    const ctx = c.getContext('2d');
    for (let i = 0; i < 50; i++) {
        ctx.save();
        ctx.fillStyle = 'red';
        ctx.fillRect(i % W, 0, 1, H);
    }
    ctx.reset();
    assert(px(ctx, 10, 10)[3] === 0, 'draws before reset() in the same turn cleared');
    ctx.fillRect(0, 0, 1, 1);
    assert(px(ctx, 0, 0)[3] === 255, 'and fifty unbalanced saves did not survive to clip anything');
    document.body.removeChild(c);
}

// ---------------------------------------------------------------------------
// canvas.width / canvas.height assignment
// ---------------------------------------------------------------------------
{
    const c = makeCanvas(W, H);
    const ctx = c.getContext('2d');
    dirty(ctx);
    c.width = W;   // the same value still resets
    checkDefaults(ctx, 'canvas.width = same');

    dirty(ctx);
    c.height = H;
    checkDefaults(ctx, 'canvas.height = same');

    dirty(ctx);
    c.setAttribute('width', String(W));
    checkDefaults(ctx, 'setAttribute("width")');

    // A real resize: the new bitmap is the new size and the state is default.
    dirty(ctx);
    c.width = 30;
    c.height = 20;
    assert(c.width === 30 && c.height === 20, 'canvas resized');
    for (const k of ATTRS) {
        assert(ctx[k] === defaults[k], `resize: ${k} back to ${defaults[k]}, got ${ctx[k]}`);
    }
    ctx.fillRect(0, 0, 30, 20);
    const corner = px(ctx, 29, 19);
    assert(corner[3] === 255 && corner[0] === 0, 'resized canvas fills to its new corner unclipped, got ' + corner);
    document.body.removeChild(c);
}

// A clip made with no save() at all is reset too.
{
    const c = makeCanvas(W, H);
    const ctx = c.getContext('2d');
    ctx.beginPath();
    ctx.rect(0, 0, 5, 5);
    ctx.clip();
    ctx.fillRect(0, 0, W, H);
    assert(px(ctx, 20, 20)[3] === 0, 'the clip applies before the reset');
    ctx.reset();
    ctx.fillRect(0, 0, W, H);
    assert(px(ctx, 20, 20)[3] === 255, 'a clip outside any save() is dropped by reset()');

    ctx.beginPath();
    ctx.rect(0, 0, 5, 5);
    ctx.clip();
    c.width = W;
    ctx.fillRect(0, 0, W, H);
    assert(px(ctx, 20, 20)[3] === 255, 'and by canvas.width assignment');
    document.body.removeChild(c);
}

// A frame boundary between the dirtying and the reset changes nothing.
{
    const c = makeCanvas(W, H);
    const ctx = c.getContext('2d');
    dirty(ctx);
    flush();
    advanceTime(16);
    ctx.reset();
    flush();
    checkDefaults(ctx, 'reset() across frames');
    document.body.removeChild(c);
}
