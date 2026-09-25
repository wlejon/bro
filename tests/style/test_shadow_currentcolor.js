// A shadow that names no colour is `currentcolor` (CSS Backgrounds 3 for
// box-shadow, Text Decoration 3 for text-shadow, Filter Effects 1 for
// drop-shadow()), so it paints in the element's own `color`.
//
// Before: box-shadow fell back to a fixed rgba(0,0,0,80/255), text-shadow to
// rgba(0,0,0,.5) and drop-shadow() to black. The shadow parsers also split a
// colour on spaces, so `rgb(0 128 0)` lent its channels to the offsets, and
// drop-shadow() read `drop-shadow(12px 0 lime)` as a blur of `lime` and
// painted it black.

document.body.style.cssText = 'margin:0;background:#fff;';
const box = (id, left, top, css) =>
    `<div id="${id}" style="position:absolute;left:${left}px;top:${top}px;` +
    `width:40px;height:40px;${css}"></div>`;
document.body.innerHTML =
    // Outset box-shadows, offset 20px right of a 40px box, no blur.
    box('bsCurrent', 40, 40, 'color:rgb(255,0,0);box-shadow:20px 0 0 0;') +
    box('bsKeyword', 140, 40, 'color:rgb(255,0,0);box-shadow:20px 0 currentcolor;') +
    box('bsNamed', 240, 40, 'color:rgb(255,0,0);box-shadow:20px 0 0 rgb(0,0,255);') +
    box('bsSpaces', 340, 40, 'box-shadow:20px 0 rgb(0 128 0 / 100%);') +
    box('bsFirst', 440, 40, 'color:rgb(0,0,255);box-shadow:rgb(255,0,0) 20px 0;') +
    // Inset: a 10px spread inset shadow on a white box, no colour.
    box('bsInset', 540, 40, 'background:#fff;color:rgb(0,0,255);box-shadow:inset 0 0 0 10px;') +
    // drop-shadow() of an opaque box, 20px right, in every colour position.
    box('dsCurrent', 40, 140, 'background:rgb(255,0,0);color:rgb(0,0,255);filter:drop-shadow(20px 0 0);') +
    box('dsLast', 140, 140, 'background:rgb(255,0,0);filter:drop-shadow(20px 0 rgb(0,255,0));') +
    box('dsFirst', 240, 140, 'background:rgb(255,0,0);filter:drop-shadow(rgb(0,255,0) 20px 0 0);') +
    // text-shadow, pushed 60px below the glyphs so the two never overlap.
    `<div id="ts" style="position:absolute;left:40px;top:240px;font:bold 48px sans-serif;` +
    `line-height:60px;color:rgb(255,0,0);text-shadow:0 60px;">MMMM</div>`;
flush();

function near(p, r, g, b, tol = 24) {
    return Math.abs(p.r - r) <= tol && Math.abs(p.g - g) <= tol && Math.abs(p.b - b) <= tol;
}
function rgb(p) { return 'rgb(' + p.r + ',' + p.g + ',' + p.b + ')'; }
function expectAt(x, y, want, msg) {
    const p = getPixel(x, y);
    assert(near(p, want[0], want[1], want[2]), msg + ': want rgb(' + want + '), got ' + rgb(p));
}

const RED = [255, 0, 0], BLUE = [0, 0, 255], GREEN = [0, 128, 0], LIME = [0, 255, 0];
// The shadow of a box at (left, top) sits in [left+40, left+60) horizontally.
const shadowAt = (left, top) => [left + 50, top + 20];

expectAt(...shadowAt(40, 40), RED, 'box-shadow with no colour paints currentcolor');
expectAt(...shadowAt(140, 40), RED, 'box-shadow: currentcolor paints the element colour');
expectAt(...shadowAt(240, 40), BLUE, 'box-shadow with a colour paints that colour');
expectAt(...shadowAt(340, 40), GREEN, 'box-shadow colour in space syntax stays one colour');
expectAt(...shadowAt(440, 40), RED, 'box-shadow colour may come first');
expectAt(545, 60, BLUE, 'inset box-shadow with no colour paints currentcolor');
expectAt(560, 60, [255, 255, 255], 'inset shadow leaves the middle of the box alone');

expectAt(...shadowAt(40, 140), BLUE, 'drop-shadow() with no colour paints currentcolor');
expectAt(...shadowAt(140, 140), LIME, 'drop-shadow() with a colour and no blur paints the colour');
expectAt(...shadowAt(240, 140), LIME, 'drop-shadow() colour may come first');
expectAt(60, 160, RED, 'the drop-shadowed box itself still paints');

// text-shadow: somewhere in the band 60px below the glyphs there must be the
// text colour, and no grey (the old rgba(0,0,0,.5) default on white).
const ts = document.getElementById('ts').getBoundingClientRect();
let red = 0, grey = 0;
for (let y = Math.round(ts.top) + 60; y < Math.round(ts.top) + 120; y += 2) {
    for (let x = Math.round(ts.left); x < Math.round(ts.right); x += 2) {
        const p = getPixel(x, y);
        if (near(p, 255, 0, 0, 40)) red++;
        else if (Math.abs(p.r - p.g) < 12 && Math.abs(p.g - p.b) < 12 && p.r < 200) grey++;
    }
}
assert(red > 20, 'text-shadow with no colour paints the text colour, red samples: ' + red);
assert(grey === 0, 'text-shadow with no colour paints no grey, grey samples: ' + grey);
