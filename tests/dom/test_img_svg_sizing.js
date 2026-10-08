// <img src="*.svg"> honours the document's intrinsic size and the CSS box.
//
// Real desktop icons the rasterizer (src/svg) used to draw wrong, each
// synthesized here in the same shape:
//  - htop: width/height in mm (+ viewBox) — a physical unit is CSS px at
//    96/in, and Skia's root, left to itself, laid the content out at its own
//    90 dpi size over the requested box (wrong scale, clipped);
//  - nvtop: width="200" with a 52.9 viewBox — the root's absolute size beat
//    the box, drawing at 200px into a 48px image;
//  - KDE breeze (Discover, Spectacle): no viewBox, minified number lists like
//    translate(-384.57-499.8) that Skia's parser rejected, so the whole
//    transform was dropped and the content drew hundreds of px off-canvas
//    (a blank box);
//  - one attribute per line (Inkscape), and a compact matrix().

const fs = require('fs');
const os = require('os');
const path = require('path');

const dir = path.join(os.tmpdir(), 'bro_svg_sizing_' + Date.now());
fs.mkdirSync(dir, { recursive: true });
function svgFile(name, text) {
  const p = path.join(dir, name);
  fs.writeFileSync(p, text);
  return p;
}

const files = {
  mm: svgFile('mm.svg',
    '<svg width="12.7mm" height="12.7mm" version="1.1" viewBox="0 0 12.7 12.7" ' +
    'xmlns="http://www.w3.org/2000/svg"><rect width="12.7" height="12.7" fill="#00ff00"/>' +
    '<rect x="6.35" y="6.35" width="6.35" height="6.35" fill="#ff0000"/></svg>'),
  big: svgFile('big.svg',
    '<svg\n   width="200"\n   height="200"\n   viewBox="0 0 50 50"\n   version="1.1"\n' +
    '   xmlns="http://www.w3.org/2000/svg">\n<rect width="50" height="50" fill="#00ff00"/>' +
    '<rect x="25" y="25" width="25" height="25" fill="#ff0000"/></svg>'),
  kde: svgFile('kde.svg',
    '<svg width="48" xmlns="http://www.w3.org/2000/svg" height="48">\n' +
    ' <g transform="translate(-384.57-499.8)">\n' +
    '  <rect width="48" x="384.57" y="499.8" fill="#0000ff" height="48"/>\n' +
    ' </g>\n</svg>'),
  matrix: svgFile('matrix.svg',
    '<svg\n   xmlns="http://www.w3.org/2000/svg"\n   width="24"\n   height="24">\n' +
    '<rect width="12" height="24" fill="#ff0000" transform="matrix(-1 0 0 1 24-0)"/></svg>'),
};

document.body.style.margin = '0';
document.body.style.background = '#ffffff';
function place(src, x, y, size) {
  const img = document.createElement('img');
  img.src = src;
  img.style.position = 'absolute';
  img.style.left = x + 'px';
  img.style.top = y + 'px';
  if (size) { img.style.width = size + 'px'; img.style.height = size + 'px'; }
  document.body.appendChild(img);
  return img;
}

const mmNatural = place(files.mm, 0, 200);
const mm = place(files.mm, 0, 0, 96);
const big = place(files.big, 100, 0, 48);
const kde = place(files.kde, 200, 0, 96);
const matrix = place(files.matrix, 300, 0, 48);
const matrixNatural = place(files.matrix, 400, 0);
flush();

// Intrinsic sizes in CSS px: 12.7mm = 48px; a viewBox never overrides width/height.
assert(mmNatural.naturalWidth === 48 && mmNatural.naturalHeight === 48,
       'mm-sized SVG is 48x48 CSS px, got ' + mmNatural.naturalWidth + 'x' + mmNatural.naturalHeight);
assert(big.naturalWidth === 200, 'width="200" is the intrinsic width, got ' + big.naturalWidth);
assert(matrixNatural.naturalWidth === 24 && matrixNatural.naturalHeight === 24,
       'one-attribute-per-line width/height parse, got ' + matrixNatural.naturalWidth + 'x' + matrixNatural.naturalHeight);
const mr = mmNatural.getBoundingClientRect();
assert(Math.round(mr.width) === 48, 'an unsized mm <img> lays out at 48px, got ' + mr.width);

function px(x, y) { const p = getPixel(x, y); return [p.r, p.g, p.b]; }
function isColor(rgb, want, what) {
  const ok = rgb.every((v, i) => Math.abs(v - want[i]) < 60);
  assert(ok, what + ': expected rgb(' + want + '), got rgb(' + rgb + ')');
}
const GREEN = [0, 255, 0], RED = [255, 0, 0], BLUE = [0, 0, 255], WHITE = [255, 255, 255];

// mm document scaled into a 96px box: green top-left quarter, red bottom-right.
isColor(px(10, 10), GREEN, 'mm: top-left');
isColor(px(86, 86), RED, 'mm: bottom-right quarter fills the box corner');
isColor(px(40, 40), GREEN, 'mm: just above the midpoint');
// The natural-size copy at y=200: 48px, red corner at 47,247.
isColor(px(44, 244), RED, 'mm natural: bottom-right at 48px');
isColor(px(52, 210), WHITE, 'mm natural: nothing past 48px');

// width=200 / viewBox 50 into 48px: whole document fits the box.
isColor(px(100 + 6, 6), GREEN, 'big: top-left');
isColor(px(100 + 42, 42), RED, 'big: bottom-right quarter is in the 48px box');

// KDE-style: the translate applies, the blue square fills the 96px box.
isColor(px(200 + 48, 48), BLUE, 'kde: centre is drawn (transform parsed)');
isColor(px(200 + 4, 4), BLUE, 'kde: top-left corner');
isColor(px(200 + 92, 92), BLUE, 'kde: bottom-right corner');

// matrix(-1 0 0 1 24-0) mirrors the left half rect to the right half.
isColor(px(300 + 36, 24), RED, 'matrix: mirrored rect on the right');
isColor(px(300 + 10, 24), WHITE, 'matrix: left half empty');

try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
console.log('img svg sizing OK');
