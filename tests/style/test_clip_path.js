// clip-path: polygon() — units, math functions, and malformed-value handling.
//
// The coordinate parser used to walk the string with a raw strtof cursor that
// made no progress on anything it didn't recognize: `calc(100% - 8px)` as a
// coordinate spun it forever, pinning a core and growing the vertex vector
// unboundedly — a windowed app with a chamfered-corner clip hung on its first
// recorded frame. Coordinates now go through the shared htmlayout length
// resolver, so px / % / font-relative units and calc()/min()/max()/clamp()
// all resolve, and any value the parser can't structure into "x y" pairs
// invalidates the whole declaration (no clip) instead of stalling.
//
// Merely completing this file is the livelock regression test; the pixel
// probes then pin down what the resolved polygons actually clip.

const root = document.getElementById('root');
document.body.style.cssText = 'margin:0;background:#000;';

function box(id, left, extra) {
    return '<div id="' + id + '" style="position:absolute;left:' + left +
           'px;top:40px;width:100px;height:100px;background:#ff0000;' +
           extra + '"></div>';
}

root.innerHTML =
    // 1. Plain px/% polygon: bottom-right triangle cut off.
    box('plain', 20,
        'clip-path:polygon(0 0, 100% 0, 0 100%);') +
    // 2. The regression: calc() chamfer on the top-right corner.
    box('chamfer', 160,
        'clip-path:polygon(0 0, calc(100% - 30px) 0, 100% 30px, 100% 100%, 0 100%);') +
    // 3. min() coordinate (its inner comma must not split the vertex list).
    box('mathfn', 300,
        'clip-path:polygon(0 0, min(100%, 200px) 0, 0 100%);') +
    // 4. Font-relative units: 5em at 20px font = the same 100px triangle.
    box('em', 440,
        'font-size:20px;clip-path:polygon(0 0, 5em 0, 0 5em);') +
    // 5. Fill-rule prefix is skipped, not treated as a vertex.
    box('fillrule', 580,
        'clip-path:polygon(evenodd, 0 0, 100% 0, 0 100%);') +
    // 6. Malformed vertex (three components) invalidates the whole value:
    //    no clip, the full box paints.
    box('invalid', 720,
        'clip-path:polygon(0 0, 10px 20px 30px, 100% 100%);');
flush();

function probe(id, relX, relY) {
    const r = document.getElementById(id).getBoundingClientRect();
    return getPixel(Math.round(r.x + relX), Math.round(r.y + relY));
}
function assertPainted(id, relX, relY, what) {
    const p = probe(id, relX, relY);
    assert(p.r > 128 && p.g < 64 && p.b < 64,
           id + ': ' + what + ' at (' + relX + ',' + relY + ') should be red, got rgb(' +
           p.r + ',' + p.g + ',' + p.b + ')');
}
function assertClipped(id, relX, relY, what) {
    const p = probe(id, relX, relY);
    assert(p.r < 64 && p.g < 64 && p.b < 64,
           id + ': ' + what + ' at (' + relX + ',' + relY + ') should be clipped to page bg, got rgb(' +
           p.r + ',' + p.g + ',' + p.b + ')');
}

// 1. Triangle polygon(0 0, 100% 0, 0 100%): interior is x+y < 100.
assertPainted('plain', 20, 20, 'triangle interior');
assertClipped('plain', 70, 70, 'cut half');

// 2. calc() chamfer: the cut line runs (70,0)→(100,30); outside is x+y > 100.
assertPainted('chamfer', 50, 50, 'center');
assertPainted('chamfer', 70, 15, 'inside the cut line');
assertClipped('chamfer', 99, 15, 'chamfered corner');
assertPainted('chamfer', 2, 2, 'untouched top-left corner');

// 3. min(100%, 200px) = 100%: same triangle as case 1.
assertPainted('mathfn', 20, 20, 'triangle interior');
assertClipped('mathfn', 70, 70, 'cut half');

// 4. 5em @ 20px = 100px: same triangle again.
assertPainted('em', 20, 20, 'triangle interior');
assertClipped('em', 70, 70, 'cut half');

// 5. Fill-rule prefix skipped: same triangle again.
assertPainted('fillrule', 20, 20, 'triangle interior');
assertClipped('fillrule', 70, 70, 'cut half');

// 6. Invalid value → declaration dropped → nothing clipped.
assertPainted('invalid', 20, 20, 'interior');
assertPainted('invalid', 95, 95, 'corner that a valid clip would cut');

// Cleanup so state doesn't leak to other tests.
root.innerHTML = '';
document.body.style.cssText = '';
