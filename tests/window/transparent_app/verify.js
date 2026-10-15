// Run by test_window_transparent.js in a child bro-headless whose app dir is
// transparent_app/ (bro.json: transparent, clickThrough). Asserts:
//   - captures of a transparent window carry real, straight alpha: what the
//     page leaves unpainted reads alpha 0, translucent CSS reads its own
//     colour at its own alpha (not blended over black);
//   - the click-through input region (inputRegion()) is exactly the points
//     the page's hit test takes (takesPointerAt), for a DOM that exercises
//     pointer-events, overflow clipping, a fixed box, a transform and the
//     <html>/<body> exemption, and it follows the DOM as it changes.

assert(bro.window.transparent === true, 'bro.json transparent applied');
assert(bro.window.clickThrough === true, 'bro.json clickThrough applied');

function near(px, r, g, b, a, what) {
    const ok = Math.abs(px.r - r) <= 2 && Math.abs(px.g - g) <= 2 &&
               Math.abs(px.b - b) <= 2 && Math.abs(px.a - a) <= 2;
    assert(ok, `${what}: expected ~(${r},${g},${b},${a}), got (${px.r},${px.g},${px.b},${px.a})`);
}

document.body.innerHTML = `
  <div id="a" style="position:absolute;left:10px;top:10px;width:50px;height:50px;
       background:rgba(255,0,0,0.5)"></div>
  <div id="b" style="position:absolute;left:100px;top:10px;width:40px;height:40px;
       background:rgb(0,0,255);pointer-events:none">
    <div id="c" style="position:absolute;left:10px;top:10px;width:10px;height:10px;
         background:#fff;pointer-events:auto"></div>
  </div>
  <div id="d" style="position:absolute;left:200px;top:10px;width:40px;height:40px;
       overflow:hidden;pointer-events:none">
    <div id="e" style="width:100px;height:100px;background:rgb(0,128,0);pointer-events:auto"></div>
  </div>
  <div id="f" style="position:fixed;left:10px;top:100px;width:30px;height:20px;background:#888"></div>
  <div id="g" style="position:absolute;left:300px;top:10px;width:40px;height:40px;
       background:#444;transform:translateX(20px)"></div>
  <div id="s" style="position:absolute;left:10px;top:200px;width:40px;height:40px;
       background:#000;box-shadow:0 0 20px rgba(0,0,0,1)"></div>`;
flush();

// ── Alpha readback ───────────────────────────────────────────────────────────
near(getPixel(380, 280), 0, 0, 0, 0, 'unpainted pixel is transparent');
near(getPixel(30, 30), 255, 0, 0, 128, 'rgba(255,0,0,0.5) reads straight, half alpha');
near(getPixel(105, 15), 0, 0, 255, 255, 'opaque blue stays opaque');
const shadow = getPixel(30, 255);
assert(shadow.a > 0 && shadow.a < 255, 'a box shadow fades out in alpha: a=' + shadow.a);
const img = frameImage();
const i = (280 * img.width + 380) * 4;
assert(img.data[i + 3] === 0, 'frameImage carries the alpha too');

// ── Point query ──────────────────────────────────────────────────────────────
const expect = [
    [30, 30, true, 'a box takes input'],
    [380, 280, false, 'nothing there: <body>/<html> pass through'],
    [105, 15, false, 'pointer-events: none passes through'],
    [115, 25, true, 'pointer-events: auto inside pointer-events: none takes input'],
    [220, 20, true, 'a box inside an overflow clip takes input inside the clip'],
    [270, 20, false, '... and passes through outside it'],
    [20, 110, true, 'a fixed box takes input'],
    [310, 20, false, 'a transformed box passes through where it no longer is'],
    [350, 20, true, '... and takes input where it moved to'],
];
for (const [x, y, want, what] of expect)
    assert(takesPointerAt(x, y) === want, `${what} (${x},${y}): takesPointerAt = ${!want}`);

// ── The region agrees with the point query everywhere ───────────────────────
function inRegion(rects, x, y) {
    return rects.some(r => x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height);
}
function checkRegion(label) {
    const rects = inputRegion();
    let mismatches = 0, first = '';
    for (let y = 0.5; y < 300; y += 3) {
        for (let x = 0.5; x < 400; x += 3) {
            const a = inRegion(rects, x, y), b = takesPointerAt(x, y);
            if (a !== b) {
                if (!mismatches) first = `(${x},${y}) region=${a} hit=${b}`;
                ++mismatches;
            }
        }
    }
    assert(mismatches === 0, `${label}: the region and the hit test disagree at ${mismatches} points, first ${first}; ` +
           JSON.stringify(rects));
    return rects;
}
let rects = checkRegion('initial DOM');
assert(rects.length > 0, 'the region is not empty');

// It follows the DOM: a box appears, another stops taking input.
const extra = document.createElement('div');
extra.style.cssText = 'position:absolute;left:150px;top:150px;width:60px;height:30px;background:#0ff';
document.body.appendChild(extra);
document.getElementById('a').style.pointerEvents = 'none';
flush();
rects = checkRegion('after the DOM changed');
assert(inRegion(rects, 160, 160), 'the new box is in the region');
assert(!inRegion(rects, 30, 30), 'the box set to pointer-events: none left the region');

// ── Runtime toggle ───────────────────────────────────────────────────────────
bro.window.clickThrough = false;
assert(bro.window.clickThrough === false, 'clickThrough turns off');
bro.window.clickThrough = true;
assert(bro.window.clickThrough === true, 'clickThrough turns back on');

console.log('TRANSPARENT_WINDOW_OK');
