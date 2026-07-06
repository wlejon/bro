// Menu inset + document scroll TOGETHER: canvas layer quads must stay glued
// to their element while the viewport scrolls under a reserved top inset.
// The refactor that moved the app document into content-space rendering
// (content-sized layer surfaces, compositor places them at (0, insetTop))
// touches exactly this composition — and the old model was never exercised
// under scroll+inset simultaneously, which is how the 28px quad drift
// shipped in the first place.
//
// Strategy: a blue canvas wrapped by a red HTML border sits at doc y=600 in
// a 3000px-tall page with the menu shown. If the canvas quad ever drifts
// from the HTML layer (the historical bug class), the blue run separates
// from its red frame. We verify pixel adjacency before AND after a wheel
// scroll, plus the absolute pre-scroll position (rect.top + inset).

const os = require('os');
const path = require('path');
const fs = require('fs');

const INSET = 28;
const CANVAS_W = 200, CANVAS_H = 120, BORDER = 4;

document.body.style.margin = '0';
document.body.style.background = 'rgb(255,255,255)';
// test_app's stylesheet pins body height to the viewport — the viewport
// scroll range comes from the root box, so let the document actually grow.
document.body.style.width = 'auto';
document.body.style.height = 'auto';
document.body.innerHTML =
    '<div style="height:600px"></div>' +
    '<div id="frame" style="box-sizing:content-box; width:' + CANVAS_W +
    'px; border:' + BORDER + 'px solid rgb(255,0,0); line-height:0;">' +
    '<canvas id="cv" style="display:block" width="' + CANVAS_W + '" height="' + CANVAS_H + '"></canvas>' +
    '</div>' +
    '<div style="height:2300px"></div>';
const cv = document.getElementById('cv');
const g = cv.getContext('2d');
g.fillStyle = 'rgb(0,0,255)';
g.fillRect(0, 0, CANVAS_W, CANVAS_H);

bro.menu.set([{ label: 'File', items: [{ id: 'noop', label: 'Noop' }] }]);
bro.menu.show();
flush();

// One render per phase: screenshot -> decode -> scan in JS.
const shots = [];
function snap(tag) {
    const p = path.join(os.tmpdir(), 'bro_menu_scroll_' + tag + '_' + Date.now() + '.png');
    screenshot(p);
    shots.push(p);
    const img = new Image();
    img.src = p;                        // sync decode in bro
    assert(img.naturalWidth > 0, 'screenshot decodes: ' + tag);
    const c = document.createElement('canvas');
    c.width = img.naturalWidth; c.height = img.naturalHeight;
    const cx = c.getContext('2d');
    cx.drawImage(img, 0, 0);
    return { data: cx.getImageData(0, 0, c.width, c.height), w: c.width, h: c.height };
}
function pxAt(shot, x, y) {
    const o = (y * shot.w + x) * 4;
    return [shot.data.data[o], shot.data.data[o + 1], shot.data.data[o + 2]];
}
const isBlue = (p) => p[2] > 200 && p[0] < 80 && p[1] < 80;
const isRed  = (p) => p[0] > 200 && p[1] < 80 && p[2] < 80;

// Find the blue quad's top edge scanning down one column.
function findQuadTop(shot, x) {
    for (let y = INSET; y < shot.h; y++) {
        if (isBlue(pxAt(shot, x, y))) return y;
    }
    return -1;
}
// The invariant that matters: the quad is exactly framed by its HTML border.
function assertGlued(shot, tag) {
    const rect = cv.getBoundingClientRect();
    const midX = Math.round(rect.left + CANVAS_W / 2);
    const top = findQuadTop(shot, midX);
    assert(top >= 0, tag + ': canvas quad visible');
    // Vertical adjacency: red border directly above the first blue row and
    // directly below the last one — zero drift tolerance.
    assert(isRed(pxAt(shot, midX, top - 2)), tag + ': border above quad, got ' + pxAt(shot, midX, top - 2));
    const bottom = top + CANVAS_H - 1;
    assert(isBlue(pxAt(shot, midX, bottom)), tag + ': quad bottom row blue, got ' + pxAt(shot, midX, bottom));
    assert(isRed(pxAt(shot, midX, bottom + 2)), tag + ': border below quad, got ' + pxAt(shot, midX, bottom + 2));
    // Horizontal adjacency at quad mid-height.
    const midY = top + Math.floor(CANVAS_H / 2);
    const left = Math.round(rect.left);
    assert(isRed(pxAt(shot, left - 2, midY)), tag + ': border left of quad');
    assert(isBlue(pxAt(shot, left + 2, midY)), tag + ': quad left edge blue');
    assert(isRed(pxAt(shot, left + CANVAS_W + 1, midY)), tag + ': border right of quad');
    return top;
}

// -- before scroll: absolute position = doc rect + menu inset ---------------
const rect0 = cv.getBoundingClientRect();
const shot0 = snap('pre');
const top0 = assertGlued(shot0, 'pre-scroll');
assert(Math.abs(top0 - (rect0.top + INSET)) <= 1,
       'pre-scroll quad top at rect.top + inset: ' + top0 + ' vs ' + (rect0.top + INSET));

// -- scroll the viewport, let the wheel smoothing drain fully ---------------
wheel(Math.round(rect0.left + CANVAS_W / 2), 500, 6);   // over plain body
for (let i = 0; i < 12; i++) { advanceTime(50); flush(); }

const shot1 = snap('post');
const top1 = assertGlued(shot1, 'post-scroll');
assert(top1 < top0 - 50,
       'viewport actually scrolled (quad moved up): ' + top0 + ' -> ' + top1);

// menu bar panel still owns the inset band after scrolling (the app content
// must never paint into it).
assert(!isBlue(pxAt(shot1, Math.round(rect0.left + CANVAS_W / 2), Math.max(0, INSET - 10))),
       'app content stays out of the menu band');

bro.menu.hide();
flush();
shots.forEach((p) => { try { fs.unlinkSync(p); } catch (e) {} });
