// Regression: Range.cloneContents() used to clone elements by serializing them
// to outerHTML and reparsing the markup. Anything a markup round-trip cannot
// express was silently dropped from the clone — a <canvas>'s backing store and
// a <select>'s live selection being the two that bite in practice. Both now go
// through the native Document::cloneNode.
//
// Dropping event listeners IS correct per spec and is asserted below so the
// native clone can't quietly start copying them.

const root = document.getElementById('root');
root.innerHTML =
    '<div id="host">' +
    '<canvas id="cv" width="8" height="8"></canvas>' +
    '<select id="sel"><option value="a">A</option><option value="b">B</option>' +
    '<option value="c">C</option></select>' +
    '<button id="btn">hi</button>' +
    '</div>';
flush();

const host = document.getElementById('host');

// --- Source state that no markup can express -------------------------------

// 1. Canvas backing store: paint it, and confirm the source really is red.
const cv = document.getElementById('cv');
const g = cv.getContext('2d');
g.fillStyle = 'rgb(255, 0, 0)';
g.fillRect(0, 0, 8, 8);
flush();
const srcPx = g.getImageData(0, 0, 1, 1).data;
assert(srcPx[0] === 255 && srcPx[1] === 0 && srcPx[2] === 0 && srcPx[3] === 255,
    'source canvas is red (' + [srcPx[0], srcPx[1], srcPx[2], srcPx[3]] + ')');

// 2. <select> selection driven through the live control, so it diverges from
//    the `selected` attributes that were parsed out of the markup.
const sel = document.getElementById('sel');
sel.selectedIndex = 2;
flush();
assert(sel.value === 'c', 'source select value diverged to "c" (got ' + sel.value + ')');

// 3. A listener, which must NOT survive the clone.
let srcClicks = 0;
document.getElementById('btn').addEventListener('click', () => { srcClicks++; });

// --- Clone the range -------------------------------------------------------

const r = document.createRange();
r.selectNodeContents(host);
const frag = r.cloneContents();
flush();

assert(frag.childNodes.length === 3, 'fragment has all three children (got ' +
    frag.childNodes.length + ')');

const cloneCv = frag.childNodes[0];
const cloneSel = frag.childNodes[1];
const cloneBtn = frag.childNodes[2];

assert(cloneCv.nodeName === 'CANVAS', 'first clone is the canvas');
assert(cloneSel.nodeName === 'SELECT', 'second clone is the select');
assert(cloneBtn.nodeName === 'BUTTON', 'third clone is the button');

// Clones are genuinely separate nodes, not the originals.
assert(cloneCv !== cv, 'cloned canvas is a distinct node');
assert(cloneSel !== sel, 'cloned select is a distinct node');

// Structure / attributes survive, same as before the fix.
assert(cloneCv.width === 8 && cloneCv.height === 8, 'cloned canvas keeps its size');
assert(cloneSel.children.length === 3, 'cloned select keeps its options');
assert(cloneBtn.textContent === 'hi', 'cloned button keeps its text');

// --- The two things the round-trip used to lose ----------------------------

// Canvas backing store: the HTML spec's canvas cloning steps require the copy's
// bitmap to be a copy of the original's. This read returned 0,0,0,0 before.
const clonePx = cloneCv.getContext('2d').getImageData(0, 0, 1, 1).data;
assert(clonePx[0] === 255 && clonePx[1] === 0 && clonePx[2] === 0 && clonePx[3] === 255,
    'cloned canvas carries the backing store (got ' +
    [clonePx[0], clonePx[1], clonePx[2], clonePx[3]] + ')');

// One rendering context per canvas: asking again must hand back the same
// object, not a fresh blank one.
assert(cloneCv.getContext('2d') === cloneCv.getContext('2d'),
    'cloned canvas has a single stable 2D context');

// <select> live selection. This was 'a' / -1 before the fix.
assert(cloneSel.value === 'c',
    'cloned select carries the diverged selection (got ' + cloneSel.value + ')');

// --- Listeners are still dropped (spec) ------------------------------------

cloneBtn.click && cloneBtn.click();
assert(srcClicks === 0, 'clone did not inherit the source listener');

// --- The source tree is untouched by cloning -------------------------------

assert(sel.value === 'c', 'source select still selected after clone');
const srcPx2 = g.getImageData(0, 0, 1, 1).data;
assert(srcPx2[0] === 255 && srcPx2[3] === 255, 'source canvas still painted after clone');
assert(host.children.length === 3, 'source subtree unchanged by cloneContents');

// --- Element.cloneNode(true) shares the same implementation -----------------

const deep = host.cloneNode(true);
const deepCv = deep.children[0];
const deepSel = deep.children[1];
assert(deepSel.value === 'c', 'cloneNode(true) carries select selection');
const deepPx = deepCv.getContext('2d').getImageData(0, 0, 1, 1).data;
assert(deepPx[0] === 255 && deepPx[3] === 255,
    'cloneNode(true) carries the canvas backing store');
assert(deep.id === '', 'cloneNode(true) still drops the id');

root.innerHTML = '';
flush();

console.log('PASS: Range.cloneContents clones natively (canvas bitmap + select ' +
    'selection preserved, listeners dropped)');
