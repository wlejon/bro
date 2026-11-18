// A viewport whose overflow is hidden has no scrollbar and does not scroll by
// wheel (CSS Overflow §3.3: <html>'s overflow, or <body>'s when <html>'s is
// visible, is the viewport's). xeokit's page (body { overflow: hidden }) with
// an out-of-flow panel taller than the window drew a viewport scrollbar down
// its right edge, over the 3D view; Chromium shows none. Script may still
// scroll a hidden viewport.

const root = document.getElementById('root');
document.documentElement.style.background = '#ffffff';
document.body.style.margin = '0';
document.body.style.overflow = 'hidden';
root.innerHTML = '<div style="height:3000px;background:#ffffff"></div>';
flush();

const w = window.innerWidth, h = window.innerHeight;
const edge = getPixels(w - 12, 0, 12, h);
let nonWhite = 0;
for (let i = 0; i < edge.data.length; i += 4)
    if (edge.data[i] < 250 || edge.data[i + 1] < 250 || edge.data[i + 2] < 250) nonWhite++;
assert(nonWhite === 0, `no viewport scrollbar along the right edge (${nonWhite} non-white pixels)`);

wheel(w / 2, h / 2, 3);
advanceTime(500);
assert(window.scrollY === 0, `the wheel does not scroll a hidden viewport (scrollY ${window.scrollY})`);

window.scrollTo(0, 200);
advanceTime(50);
assert(window.scrollY === 200, `script still scrolls it (scrollY ${window.scrollY})`);
wheel(w / 2, h / 2, 3);
advanceTime(500);
assert(window.scrollY === 200, `and the wheel leaves that offset alone (scrollY ${window.scrollY})`);

// The same page with a visible overflow scrolls and shows its scrollbar.
window.scrollTo(0, 0);
document.body.style.overflow = '';
flush();
wheel(w / 2, h / 2, 3);
advanceTime(500);
assert(window.scrollY > 0, `a visible viewport scrolls by wheel (scrollY ${window.scrollY})`);
