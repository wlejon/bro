// Canvas2D text against the shaper: the full TextMetrics surface, the
// `direction` attribute, and direction-relative text alignment.
//
// Canvas used to have its own text stack — SkFont::measureText for widths and
// drawSimpleText for glyphs, both 1:1 codepoint→glyph. That could not kern,
// could not ligate, and drew Arabic and Hebrew in logical (wrong) order. These
// assertions are the observable consequences of it going through the same
// shaper as everything else.

var canvas = document.createElement('canvas');
canvas.setAttribute('width', '400');
canvas.setAttribute('height', '200');
document.body.appendChild(canvas);
flush();

var ctx = canvas.getContext('2d');
ctx.font = '40px Arial';

// --- TextMetrics: the whole spec surface, not just three fields ---
var m = ctx.measureText('Hello');
var required = [
    'width',
    'actualBoundingBoxLeft', 'actualBoundingBoxRight',
    'actualBoundingBoxAscent', 'actualBoundingBoxDescent',
    'fontBoundingBoxAscent', 'fontBoundingBoxDescent',
    'emHeightAscent', 'emHeightDescent',
    'hangingBaseline', 'alphabeticBaseline', 'ideographicBaseline'
];
for (var i = 0; i < required.length; i++) {
    assert(typeof m[required[i]] === 'number',
           'TextMetrics.' + required[i] + ' is a number');
}
assert(m.width > 0, 'measureText width is positive');

// The font box contains the ink box: a face's declared ascent is at least the
// ascent of any glyph drawn in it.
assert(m.fontBoundingBoxAscent >= m.actualBoundingBoxAscent - 0.01,
       'font ascent bounds ink ascent (' + m.fontBoundingBoxAscent +
       ' >= ' + m.actualBoundingBoxAscent + ')');

// "Hello" has no descender, so its ink never crosses the baseline. This is the
// assertion that fails if actualBoundingBox* is silently reporting font
// metrics instead of measuring the glyphs asked for.
assert(m.actualBoundingBoxDescent < 1.0,
       'no-descender string reports ~zero ink descent, got ' + m.actualBoundingBoxDescent);
var mg = ctx.measureText('gjpqy');
assert(mg.actualBoundingBoxDescent > 1.0,
       'descender string reports real ink descent, got ' + mg.actualBoundingBoxDescent);

// The em box sums to the font size by construction.
assert(Math.abs((m.emHeightAscent + m.emHeightDescent) - 40) < 0.5,
       'emHeightAscent + emHeightDescent == font size, got ' +
       (m.emHeightAscent + m.emHeightDescent));

// With the default alphabetic baseline the alignment point IS the baseline.
assert(Math.abs(m.alphabeticBaseline) < 0.01,
       'alphabeticBaseline is 0 for textBaseline=alphabetic');
assert(m.hangingBaseline > 0, 'hanging baseline sits above the alphabetic one');
assert(m.ideographicBaseline < 0, 'ideographic baseline sits below it');

// --- metrics move with textAlign, because they are relative to the anchor ---
ctx.textAlign = 'start';
var mStart = ctx.measureText('Hello');
ctx.textAlign = 'center';
var mCenter = ctx.measureText('Hello');
ctx.textAlign = 'right';
var mRight = ctx.measureText('Hello');
ctx.textAlign = 'start';

assert(Math.abs(mStart.width - mCenter.width) < 0.01,
       'textAlign does not change the width');
assert(mCenter.actualBoundingBoxLeft > mStart.actualBoundingBoxLeft,
       'centred text reaches further left of the anchor than start-aligned');
assert(mRight.actualBoundingBoxLeft > mCenter.actualBoundingBoxLeft,
       'right-aligned text reaches further left still');

// --- textAlign round-trips every keyword ---
// `left` and `start` used to share one code, so assigning "left" read back as
// "start". They are different things once a direction exists to resolve
// `start` against.
var aligns = ['start', 'center', 'right', 'end', 'left'];
for (var j = 0; j < aligns.length; j++) {
    ctx.textAlign = aligns[j];
    assert(ctx.textAlign === aligns[j],
           'textAlign round-trips "' + aligns[j] + '", got "' + ctx.textAlign + '"');
}
ctx.textAlign = 'start';

// --- direction ---
assert(ctx.direction === 'ltr', 'direction defaults to ltr');
ctx.direction = 'rtl';
assert(ctx.direction === 'rtl', 'direction accepts rtl');
ctx.direction = 'inherit';
assert(ctx.direction === 'inherit', 'direction accepts inherit');
ctx.direction = 'ltr';

// --- shaping actually happened ---
// Kerning is the cleanest proof: an "AV" pair is narrower than A and V drawn
// independently. A 1:1 codepoint mapping sums per-glyph advances and cannot
// produce this.
var wA = ctx.measureText('A').width;
var wV = ctx.measureText('V').width;
var wAV = ctx.measureText('AV').width;
assert(wAV < wA + wV - 0.5,
       'AV kerns tighter than A+V (' + wAV + ' < ' + (wA + wV) + ')');

// Width is a property of the glyphs, not of the base direction: the same
// string measures the same either way. What the base changes is order and
// neutral resolution, which width does not see.
ctx.direction = 'ltr';
var hebLtr = ctx.measureText('שלום').width;
ctx.direction = 'rtl';
var hebRtl = ctx.measureText('שלום').width;
assert(Math.abs(hebLtr - hebRtl) < 0.01,
       'base direction does not change a uniform run width');
assert(hebRtl > 0, 'Hebrew shapes to a positive width');
ctx.direction = 'ltr';

// --- drawing still works through the blob path ---
// Text commands now carry a pre-shaped SkTextBlob rather than a string; this
// is the smoke test that the replay path draws it.
ctx.fillStyle = 'white';
ctx.fillRect(0, 0, 400, 200);
ctx.fillStyle = 'black';
ctx.fillText('Waffle office', 10, 50);
ctx.strokeText('Stroked', 10, 100);
ctx.direction = 'rtl';
ctx.fillText('שלום עולם', 380, 150);
ctx.direction = 'ltr';
flush();

var data = ctx.getImageData(0, 0, 400, 200).data;
var dark = 0;
for (var p = 0; p < data.length; p += 4) {
    if (data[p] < 128 && data[p + 3] > 0) dark++;
}
assert(dark > 100, 'text drew visible glyphs through the blob path, ' + dark + ' dark pixels');

console.log('canvas text shaping: all assertions passed');
