// Regression: with a menu bar shown, canvas/WebGL/scene layer quads and the
// <select> dropdown must composite at getBoundingClientRect() + contentTop(),
// i.e. exactly where the surrounding HTML painted. Commit 2ae5786 switched
// the layer-break quads to document-space dom::absoluteContentBox() and
// dropped the (contentTop − scrollY) draw offset, so every such quad landed
// 28px above its element in any app with a menu bar (krea2-lab's "black bar
// under every image"). Headless used to force the menu off, which is why no
// headless test could see this — test_menu_inset.js covers that.

// getPixel() is document-space — the same origin getBoundingClientRect()
// reports in — so the assertions below compare a probe directly against a
// rect with no inset arithmetic. That is the whole claim being tested: a
// layer quad lands exactly where the surrounding HTML painted it. (This test
// used to add MenuBar::height to every rect by hand, back when getPixel took
// raw frame coordinates.)

bro.menu.show();

document.body.style.margin = '0';
document.body.style.background = '#404040';
document.body.innerHTML =
  '<canvas id="c" width="100" height="100" ' +
  '  style="display:block; margin-top:50px; width:100px; height:100px"></canvas>' +
  '<div style="transform: translate(40px, 20px); width: 100px">' +
  '<canvas id="t" width="80" height="80" ' +
  '  style="display:block; width:80px; height:80px"></canvas>' +
  '</div>' +
  '<select id="s" style="width:120px; margin-top:40px">' +
  '<option>alpha</option><option>beta</option></select>';

const c = document.getElementById('c');
let g = c.getContext('2d');
g.fillStyle = '#0000ff';
g.fillRect(0, 0, 100, 100);

const t = document.getElementById('t');
g = t.getContext('2d');
g.fillStyle = '#00c800';
g.fillRect(0, 0, 80, 80);

flush();

const near = (v, want) => Math.abs(v - want) < 40;
const isBlue  = (p) => near(p.r, 0)   && near(p.g, 0)   && near(p.b, 255);
const isGreen = (p) => near(p.r, 0)   && near(p.g, 200) && near(p.b, 0);
const isBg    = (p) => near(p.r, 64)  && near(p.g, 64)  && near(p.b, 64);
// The dropdown's highlighted row — also guards the cfromColor8 fix in
// DropdownOverlay::draw (raw 8-bit ints into float Color rendered cyan).
const isSelBlue = (p) => near(p.r, 0) && near(p.g, 120) && near(p.b, 215);
const px = (x, y) => getPixel(Math.round(x), Math.round(y));
const show = (p) => p.r + ',' + p.g + ',' + p.b;

// --- plain canvas: quad exactly at rect + contentTop -----------------------
const rc = c.getBoundingClientRect();
assert(rc.y === 50, 'canvas doc-space rect.y is 50, got ' + rc.y);
const cy = rc.y;   // document-space top; no inset arithmetic needed

let p = px(10, cy - 6);
assert(isBg(p), 'no canvas content above the element box, got ' + show(p));
p = px(10, cy + 6);
assert(isBlue(p), 'canvas content at the top of the element box, got ' + show(p));
p = px(10, cy + 94);
assert(isBlue(p), 'canvas content at the bottom of the element box, got ' + show(p));
p = px(10, cy + 106);
assert(isBg(p), 'no canvas content below the element box, got ' + show(p));

// --- canvas under a CSS-transformed ancestor (2ae5786's original fix) ------
const rt = t.getBoundingClientRect();   // includes the translate(40px, 20px)
p = px(rt.x + 40, rt.y + 40);
assert(isGreen(p), 'transformed canvas centered at rect + inset, got ' + show(p));
p = px(rt.x - 8, rt.y + 40);
assert(isBg(p), 'no content left of the transformed canvas, got ' + show(p));
p = px(rt.x + 40, rt.y - 8);
assert(isBg(p), 'no content above the transformed canvas, got ' + show(p));

// --- <select> dropdown overlay anchors below the select --------------------
const rs = document.getElementById('s').getBoundingClientRect();
const belowY = rs.bottom + 5;   // just inside where the list must open
p = px(rs.x + 8, belowY);
assert(isBg(p), 'below the select before click is page background, got ' + show(p));

click(rs.x + 8, rs.y + rs.height / 2);   // input sim folds contentTop itself
flush();

p = px(rs.x + 8, belowY);
assert(isSelBlue(p), 'dropdown (highlighted first option) opens directly ' +
       'below the select, got ' + show(p));
