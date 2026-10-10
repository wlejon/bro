// Image sampling (docs/file-api.js): an <img> or a CSS background scaled up
// is filtered (linear), scaled down it is mipmapped, and
// `image-rendering: pixelated` / `crisp-edges` switch to nearest-neighbour.
// <canvas> drawImage keeps its own imageSmoothingEnabled rule.

const fs = require('fs');
const path = require('path');
const os = require('os');

const dir = path.join(os.tmpdir(), 'bro_img_sampling_' + process.pid + '_' + Date.now());
fs.mkdirSync(dir, { recursive: true });

function checker(n, cell) {
    const px = new Uint8Array(n * n * 4);
    for (let y = 0; y < n; y++) {
        for (let x = 0; x < n; x++) {
            const v = ((Math.floor(x / cell) + Math.floor(y / cell)) & 1) ? 255 : 0;
            const i = (y * n + x) * 4;
            px[i] = px[i + 1] = px[i + 2] = v;
            px[i + 3] = 255;
        }
    }
    return px;
}
function writePng(name, n, cell) {
    const p = path.join(dir, name).replace(/\\/g, '/');
    assert(bro.image.encodePngFile(p, checker(n, cell), n, n, 4), 'wrote ' + name);
    return p;
}
const BIG = writePng('big2.png', 2, 1);      // 2x2 checker, scaled up 100x
const FINE = writePng('fine.png', 256, 1);   // 1px checker, scaled down 8x

document.body.style.margin = '0';
document.body.style.background = 'rgb(255,0,0)';
document.body.innerHTML =
    '<img id="up" src="' + BIG + '" style="position:absolute;left:0;top:0;width:200px;height:200px">' +
    '<img id="upPx" src="' + BIG + '" style="position:absolute;left:220px;top:0;width:200px;height:200px;image-rendering:pixelated">' +
    '<img id="upCe" src="' + BIG + '" style="position:absolute;left:440px;top:0;width:200px;height:200px;image-rendering:crisp-edges">' +
    '<div id="bgUp" style="position:absolute;left:0;top:220px;width:200px;height:200px;background:url(' + BIG + ') 0 0 / 200px 200px no-repeat"></div>' +
    '<div id="bgUpPx" style="position:absolute;left:220px;top:220px;width:200px;height:200px;image-rendering:pixelated;background:url(' + BIG + ') 0 0 / 200px 200px no-repeat"></div>' +
    '<img id="down" src="' + FINE + '" style="position:absolute;left:0;top:440px;width:32px;height:32px">' +
    '<img id="downPx" src="' + FINE + '" style="position:absolute;left:220px;top:440px;width:32px;height:32px;image-rendering:pixelated">';
flush();

function lum(p) { return (p.r + p.g + p.b) / 3; }
function isGray(p) { return lum(p) > 60 && lum(p) < 195 && Math.abs(p.r - p.b) < 30; }
function isPure(p) { return (lum(p) < 20 || lum(p) > 235) && Math.abs(p.r - p.b) < 30; }

// Scaled up: the seam between two texels is a blend when smooth, a hard edge
// when pixelated. Texel centres sit at 50 and 150 css px; 100 is half-way.
const up = getPixel(100, 50), upPx = getPixel(320, 50), upCe = getPixel(540, 50);
assert(isGray(up), '<img> scaled up is filtered (gray between texels), got ' + JSON.stringify(up));
assert(isPure(upPx), 'image-rendering:pixelated is nearest (no blend), got ' + JSON.stringify(upPx));
assert(isPure(upCe), 'image-rendering:crisp-edges is nearest (no blend), got ' + JSON.stringify(upCe));
// The texel centres are the texel's own colour either way.
assert(lum(getPixel(50, 50)) < 20 && lum(getPixel(150, 50)) > 235, 'smooth keeps texel centres');

const bgUp = getPixel(100, 270), bgUpPx = getPixel(320, 270);
assert(isGray(bgUp), 'a background scaled up is filtered, got ' + JSON.stringify(bgUp));
assert(isPure(bgUpPx), 'a pixelated background is nearest, got ' + JSON.stringify(bgUpPx));

// Scaled down 8x: a 1px checker averages to mid-gray with mipmaps everywhere,
// while nearest picks single texels (black or white).
const down = getPixels(4, 444, 24, 24);
let grayCount = 0;
for (let i = 0; i < down.data.length; i += 4) {
    const p = { r: down.data[i], g: down.data[i + 1], b: down.data[i + 2] };
    if (lum(p) > 90 && lum(p) < 165) grayCount++;
}
assert(grayCount === 24 * 24, '<img> scaled down is mipmapped to an even gray: ' + grayCount + '/576 gray');
const downPx = getPixels(224, 444, 24, 24);
let pureCount = 0;
for (let i = 0; i < downPx.data.length; i += 4) {
    const p = { r: downPx.data[i], g: downPx.data[i + 1], b: downPx.data[i + 2] };
    if (isPure(p)) pureCount++;
}
assert(pureCount === 24 * 24, 'a pixelated <img> scaled down picks texels: ' + pureCount + '/576 pure');

// <canvas> drawImage: smoothing on by default, off with imageSmoothingEnabled.
const img = document.getElementById('up');
await img.decode();
const cv = document.createElement('canvas');
cv.width = 200; cv.height = 100;
const ctx = cv.getContext('2d');
ctx.drawImage(img, 0, 0, 100, 100);
ctx.imageSmoothingEnabled = false;
ctx.drawImage(img, 100, 0, 100, 100);
const d = ctx.getImageData(0, 0, 200, 100).data;
const at = (x, y) => { const o = (y * 200 + x) * 4; return { r: d[o], g: d[o + 1], b: d[o + 2] }; };
assert(isGray(at(50, 25)), 'drawImage smooths by default, got ' + JSON.stringify(at(50, 25)));
assert(isPure(at(150, 25)), 'drawImage with imageSmoothingEnabled=false is nearest, got ' + JSON.stringify(at(150, 25)));

fs.rmSync(dir, { recursive: true, force: true });
console.log('PASS');
