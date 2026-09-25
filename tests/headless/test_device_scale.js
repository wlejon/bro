// High-DPI rendering: setDeviceScaleFactor(s) makes headless render as a
// display with s device px per CSS px (what a Retina window does). CSS px stay
// the unit of layout, events and getPixel; devicePixelRatio, @media
// (resolution), matchMedia change listeners and the captured frame follow the
// scale. The frame is composited from layer surfaces allocated at s× the CSS
// size, so a 1 CSS px line is s device px of solid ink rather than an upscaled
// blur, and text carries ~s² as many inked pixels.

const os = require('os');
const path = require('path');
const fs = require('fs');

const root = document.getElementById('root');
const tmpDir = os.tmpdir();

// PNG width/height straight from the IHDR chunk (big-endian at bytes 16..23).
function pngSize(p) {
    const b = fs.readFileSync(p);
    const u32 = (o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
    return { w: u32(16), h: u32(20) };
}

// Decode a PNG to RGBA through the web path: <img> + 2D canvas.
function pngPixels(p) {
    const img = new Image();
    img.src = p;
    const c = document.createElement('canvas');
    c.width = img.width;
    c.height = img.height;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0);
    return { w: c.width, h: c.height, data: ctx.getImageData(0, 0, c.width, c.height).data };
}

function shot(name, selector) {
    const p = path.join(tmpDir, 'bro_test_dpr_' + name + '_' + Date.now() + '.png');
    if (selector) screenshot(p, selector); else screenshot(p);
    return p;
}

// Count pixels darker than mid-grey, and the ones that are neither ink nor
// background (antialiasing / blur).
function inkStats(px) {
    let ink = 0, grey = 0;
    for (let i = 0; i < px.data.length; i += 4) {
        const v = px.data[i];
        if (v < 128) ink++;
        if (v > 16 && v < 240) grey++;
    }
    return { ink, grey };
}

// --- baseline: 1x ------------------------------------------------------------
assert(window.devicePixelRatio === 1, 'headless default devicePixelRatio is 1, got ' + window.devicePixelRatio);
assert(matchMedia('(resolution: 1dppx)').matches, 'resolution 1dppx matches at 1x');
assert(!matchMedia('(min-resolution: 2dppx)').matches, 'min-resolution 2dppx fails at 1x');

const W = window.innerWidth, H = window.innerHeight;

const style = document.createElement('style');
style.textContent = `
  #t { color: black; }
  @media (min-resolution: 2dppx) { #t { color: rgb(0, 0, 255); } }
`;
document.head.appendChild(style);

root.innerHTML =
    '<div id="t" style="position:absolute;left:10px;top:10px;font:20px sans-serif;' +
    'background:white;padding:4px">Hello HiDPI</div>' +
    '<div id="line" style="position:absolute;left:40px;top:60px;width:1px;height:20px;background:black"></div>' +
    '<div id="pad" style="position:absolute;left:30px;top:55px;width:21px;height:30px;background:white"></div>' +
    '<button id="b" style="position:absolute;left:100px;top:100px;width:80px;height:40px">b</button>';
// The line sits over the white pad (later in paint order would hide it, so
// raise the line above it).
document.getElementById('line').style.zIndex = '2';
flush();

assert(getComputedStyle(document.getElementById('t')).color === 'rgb(0, 0, 0)',
       '@media (min-resolution: 2dppx) block does not apply at 1x');

const full1 = pngSize(shot('full1'));
assert(full1.w === W && full1.h === H, `1x frame is the viewport, got ${full1.w}x${full1.h}`);

const text1 = inkStats(pngPixels(shot('text1', '#t')));
const pad1 = pngPixels(shot('pad1', '#pad'));
assert(pad1.w === 21, '1x crop of a 21px box is 21px wide, got ' + pad1.w);

// --- matchMedia change listener fires on a scale change ---------------------
const mql = matchMedia('(min-resolution: 2dppx)');
let changes = [];
mql.addEventListener('change', (e) => changes.push(e.matches));
let resizes = 0;
window.addEventListener('resize', () => resizes++);

setDeviceScaleFactor(2);

assert(window.devicePixelRatio === 2, 'devicePixelRatio follows the scale, got ' + window.devicePixelRatio);
assert(window.innerWidth === W && window.innerHeight === H, 'CSS viewport is unchanged by the scale');
assert(matchMedia('(resolution: 2dppx)').matches, 'resolution 2dppx matches at 2x');
assert(matchMedia('(-webkit-min-device-pixel-ratio: 2)').matches, '-webkit-min-device-pixel-ratio matches at 2x');
assert(mql.matches, 'the existing list re-evaluates live');
assert(changes.length === 1 && changes[0] === true,
       'change listener fired once with matches=true, got ' + JSON.stringify(changes));
assert(resizes >= 1, 'a scale change dispatches resize');
assert(getComputedStyle(document.getElementById('t')).color === 'rgb(0, 0, 255)',
       '@media (min-resolution: 2dppx) block applies at 2x');

// Same geometry in CSS px.
const r = document.getElementById('line').getBoundingClientRect();
assert(r.left === 40 && r.width === 1, 'layout stays in CSS px at 2x');

// --- the frame is in device px ----------------------------------------------
const full2 = pngSize(shot('full2'));
assert(full2.w === 2 * W && full2.h === 2 * H,
       `2x frame is twice the viewport, got ${full2.w}x${full2.h}`);

// The 1 CSS px line is 2 device columns of solid ink over white: crisp.
const pad2 = pngPixels(shot('pad2', '#pad'));
assert(pad2.w === 42 && pad2.h === 60, `2x crop of a 21x30 box is 42x60, got ${pad2.w}x${pad2.h}`);
{
    const row = 20;  // mid-height, device px
    const cols = [];
    for (let x = 0; x < pad2.w; x++) cols.push(pad2.data[(row * pad2.w + x) * 4]);
    const black = cols.filter((v) => v < 16).length;
    const grey = cols.filter((v) => v >= 16 && v < 240).length;
    assert(black === 2, 'a 1 CSS px line is 2 solid device px at 2x, got ' + black + ' (' + cols.join(',') + ')');
    assert(grey === 0, 'and has no blurred edge, got ' + grey + ' grey px');
}

// Text is rasterized at 2x: roughly 4x the ink of the 1x glyphs (an upscaled
// 1x raster would also give ~4x, so check the grey fringe shrinks relative to
// the ink too — a blurred upscale doubles it).
document.getElementById('t').style.color = 'black';
flush();
const text2 = inkStats(pngPixels(shot('text2', '#t')));
const inkRatio = text2.ink / text1.ink;
assert(inkRatio > 3 && inkRatio < 5, 'text ink scales ~4x at 2x, got ' + inkRatio.toFixed(2));
const fringe1 = text1.grey / text1.ink, fringe2 = text2.grey / text2.ink;
assert(fringe2 < fringe1, `text edges are sharper at 2x (grey/ink ${fringe2.toFixed(2)} vs ${fringe1.toFixed(2)})`);

// getPixel still takes CSS coordinates.
const onLine = getPixel(40, 70);
assert(onLine.r < 16, 'getPixel(40,70) lands on the 1px line at 2x, got r=' + onLine.r);
const offLine = getPixel(42, 70);
assert(offLine.r > 240, 'getPixel(42,70) is the white pad, got r=' + offLine.r);

// --- pointer events stay in CSS px -------------------------------------------
let clickAt = null;
document.getElementById('b').addEventListener('click', (e) => { clickAt = [e.clientX, e.clientY]; });
click(120, 110);
assert(clickAt && clickAt[0] === 120 && clickAt[1] === 110,
       'click at CSS (120,110) hits the button at 2x with CSS clientX/Y, got ' + JSON.stringify(clickAt));

// --- canvas keeps HTML semantics: backing = canvas.width ---------------------
root.insertAdjacentHTML('beforeend', '<canvas id="cv" width="50" height="40" style="width:50px;height:40px"></canvas>');
flush();
const cv = document.getElementById('cv');
assert(cv.width === 50 && cv.height === 40, 'canvas backing size is its width/height attributes at 2x');

// --- the 3D scene renders at device px ----------------------------------------
if (typeof bro !== 'undefined' && bro.gpu !== undefined) {
    root.insertAdjacentHTML('beforeend',
        '<canvas id="sc" style="position:absolute;left:0;top:200px;width:120px;height:90px"></canvas>');
    flush();
    const scene = document.getElementById('sc').getContext('scene');
    if (scene) {
        scene.setCamera({ fov: 60, near: 0.1, far: 100,
                          position: [0, 1, 4], target: [0, 0, 0], up: [0, 1, 0] });
        scene.createMesh({ mesh: Mesh.box(1, 1, 1), color: [1, 0, 0, 1] });
        const img = scene.captureFrame(120, 90);
        assert(img && img.width === 240 && img.height === 180,
               'scene target is 2x the canvas box at 2x, got ' + (img && img.width) + 'x' + (img && img.height));
        assert(img.data.length === img.width * img.height * 4, 'scene readback size matches its dims');
    }
}

// --- back to 1x ---------------------------------------------------------------
setDeviceScaleFactor(1);
assert(window.devicePixelRatio === 1, 'devicePixelRatio returns to 1');
assert(changes.length === 2 && changes[1] === false,
       'change listener fired again with matches=false, got ' + JSON.stringify(changes));
const full3 = pngSize(shot('full3'));
assert(full3.w === W && full3.h === H, `frame is the viewport again at 1x, got ${full3.w}x${full3.h}`);
