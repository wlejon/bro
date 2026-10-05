// What a captured frame is made of, under the page's own colours:
//   - a frame is opaque: what nothing covers is black, and a translucent
//     canvas over a transparent page reads as the colour blended over black
//     (alpha 255), never as premultiplied pixels with alpha below 255 that a
//     straight-alpha PNG would darken;
//   - advanced blend modes (CSS mix-blend-mode and canvas
//     globalCompositeOperation) compute the blended colour on the GPU, and do
//     so without a Vulkan validation error (the runner fails the test on one).

document.documentElement.style.background = 'transparent';
document.body.style.cssText = 'margin:0;background:transparent';

function near(px, r, g, b, a, what) {
    const ok = Math.abs(px.r - r) <= 3 && Math.abs(px.g - g) <= 3 &&
               Math.abs(px.b - b) <= 3 && px.a === a;
    assert(ok, `${what}: expected ~(${r},${g},${b},${a}), got (${px.r},${px.g},${px.b},${px.a})`);
}

// ── Opaque frames ────────────────────────────────────────────────────────────
const c = document.createElement('canvas');
c.width = 40; c.height = 40;
c.style.cssText = 'position:absolute;left:0;top:0';
document.body.appendChild(c);
const ctx = c.getContext('2d');
ctx.fillStyle = 'rgba(255,0,0,0.5)';
ctx.fillRect(0, 0, 40, 40);
flush();
near(getPixel(100, 100), 0, 0, 0, 255, 'uncovered frame is opaque black');
near(getPixel(20, 20), 128, 0, 0, 255, 'translucent red over black');

// ── mix-blend-mode ───────────────────────────────────────────────────────────
c.remove();
document.body.innerHTML =
    '<div style="position:absolute;left:0;top:0;width:60px;height:60px;background:rgb(255,128,64)"></div>' +
    '<div style="position:absolute;left:0;top:0;width:60px;height:60px;background:rgb(128,128,255);' +
    'mix-blend-mode:multiply"></div>' +
    '<div style="position:absolute;left:100px;top:0;width:60px;height:60px;background:rgb(64,64,64)"></div>' +
    '<div style="position:absolute;left:100px;top:0;width:60px;height:60px;background:rgb(64,128,192);' +
    'mix-blend-mode:screen"></div>';
flush();
// multiply: a*b/255; screen: a + b - a*b/255.
near(getPixel(30, 30), 128, 64, 64, 255, 'mix-blend-mode: multiply');
near(getPixel(130, 30), 112, 160, 208, 255, 'mix-blend-mode: screen');

// ── canvas globalCompositeOperation ─────────────────────────────────────────
document.body.innerHTML = '';
const k = document.createElement('canvas');
k.width = 40; k.height = 40;
k.style.cssText = 'position:absolute;left:0;top:0';
document.body.appendChild(k);
const g = k.getContext('2d');
g.fillStyle = 'rgb(255,128,64)';
g.fillRect(0, 0, 40, 40);
g.globalCompositeOperation = 'multiply';
g.fillStyle = 'rgb(128,128,255)';
g.fillRect(0, 0, 40, 40);
g.globalCompositeOperation = 'difference';
g.fillStyle = 'rgb(255,255,255)';
g.fillRect(20, 0, 20, 40);
flush();
near(getPixel(10, 20), 128, 64, 64, 255, 'globalCompositeOperation multiply');
near(getPixel(30, 20), 127, 191, 191, 255, 'globalCompositeOperation difference');
const d = g.getImageData(10, 20, 1, 1).data;
assert(Math.abs(d[0] - 128) <= 3 && Math.abs(d[1] - 64) <= 3 && d[3] === 255,
       `getImageData sees the blended canvas: ${Array.from(d).join(',')}`);

console.log('frame blend/alpha tests passed');
