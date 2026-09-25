// measureText's actualBoundingBox* for a colour-bitmap glyph is the box the
// bitmap is drawn into, not the scaler's padded mask box.
//
// A glyph with no outline (Apple Color Emoji's sbix, Noto's CBDT, a COLR
// layer stack) has only mask bounds to measure, and CoreText pads those: it
// rounds out to whole pixels and then outsets one more pixel for LCD
// smoothing, so a 48 px emoji measured 51x51 — about 3 px bigger than the
// bitmap in each dimension, and integral where outline ink is fractional.
//
// The emoji face differs per platform (and may be missing), so nothing here
// names a number from one font: each string's measured box is checked against
// the ink fillText actually puts on the canvas, which is what the box claims
// to describe.

var SIZE = 200;
var ORIGIN = 100;

function drawnInk(text, px) {
    var c = document.createElement('canvas');
    c.width = SIZE;
    c.height = SIZE;
    var g = c.getContext('2d');
    g.font = px + 'px Arial';
    g.fillText(text, ORIGIN, ORIGIN);
    var d = g.getImageData(0, 0, SIZE, SIZE).data;
    var x0 = SIZE, x1 = -1, y0 = SIZE, y1 = -1;
    for (var y = 0; y < SIZE; y++) {
        for (var x = 0; x < SIZE; x++) {
            if (d[(y * SIZE + x) * 4 + 3] === 0) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < 0) return null;
    // Relative to the anchor, in the same sense as TextMetrics' fields.
    return { left: ORIGIN - x0, right: x1 + 1 - ORIGIN,
             ascent: ORIGIN - y0, descent: y1 + 1 - ORIGIN };
}

function measured(text, px) {
    var g = document.createElement('canvas').getContext('2d');
    g.font = px + 'px Arial';
    var m = g.measureText(text);
    return { left: m.actualBoundingBoxLeft, right: m.actualBoundingBoxRight,
             ascent: m.actualBoundingBoxAscent, descent: m.actualBoundingBoxDescent,
             width: m.width };
}

// The measured box covers the drawn ink and overshoots it by less than a
// pixel on each side (antialiasing can light a pixel the ink only grazes).
function checkAgainstInk(text, px) {
    var ink = drawnInk(text, px);
    var m = measured(text, px);
    if (!ink) return null;
    var sides = ['left', 'right', 'ascent', 'descent'];
    for (var i = 0; i < sides.length; i++) {
        var s = sides[i];
        assert(Math.abs(m[s] - ink[s]) <= 1.0,
               JSON.stringify(text) + ' at ' + px + 'px: measured ' + s + ' ' +
               m[s] + ' is the drawn ink ' + s + ' ' + ink[s] + ' within 1px');
    }
    return m;
}

// Outline control: this was right before and must stay right.
checkAgainstInk('Hg', 48);

var sizes = [24, 48, 64];
for (var k = 0; k < sizes.length; k++) {
    var px = sizes[k];
    var m = checkAgainstInk('\u{1F600}', px);
    if (!m) {
        console.log('no emoji ink drawn at ' + px + 'px; skipping its size check');
        continue;
    }
    // An emoji fills about one em: its box is no wider or taller than the
    // font size plus a pixel. The padded mask box was ~3 px over.
    assert(m.left + m.right <= px + 1,
           'emoji ink width ' + (m.left + m.right) + ' <= ' + px + 'px + 1');
    assert(m.ascent + m.descent <= px + 1,
           'emoji ink height ' + (m.ascent + m.descent) + ' <= ' + px + 'px + 1');
    assert(m.left + m.right >= px * 0.75 && m.ascent + m.descent >= px * 0.75,
           'emoji ink box is roughly an em, got ' + (m.left + m.right) + 'x' +
           (m.ascent + m.descent) + ' at ' + px + 'px');
}

console.log('canvas measureText bitmap glyph: all assertions passed');
