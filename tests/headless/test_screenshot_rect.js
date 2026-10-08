// screenshot(path, x, y, w, h) saves just that rect of the frame, in document
// CSS px like getPixels. The binding only knew screenshot(path) and
// screenshot(path, selector); a numeric second argument fell through to the
// full-frame form.

const fs = require('fs');
const os = require('os');
const path = require('path');

document.body.style.cssText = 'margin:0;background:rgb(255,255,255)';
document.body.innerHTML =
    '<div style="position:absolute;left:30px;top:40px;width:50px;height:20px;background:rgb(255,0,0)"></div>' +
    '<div style="position:absolute;left:80px;top:40px;width:50px;height:20px;background:rgb(0,0,255)"></div>';
flush();

// Width and height from the PNG's IHDR chunk (big-endian at bytes 16..23).
function pngSize(file) {
    const b = fs.readFileSync(file);
    const u = new Uint8Array(b.buffer ? b.buffer : b, b.byteOffset || 0, b.length);
    const be = (o) => ((u[o] << 24) | (u[o + 1] << 16) | (u[o + 2] << 8) | u[o + 3]) >>> 0;
    return { w: be(16), h: be(20), bytes: u };
}

const dir = os.tmpdir();
const scale = (typeof devicePixelRatio === 'number' && devicePixelRatio > 0) ? devicePixelRatio : 1;

const rectPath = path.join(dir, 'bro_test_rect_' + Date.now() + '.png');
assert(screenshot(rectPath, 30, 40, 100, 20) === true, 'screenshot(path, x, y, w, h) returns true');
const s = pngSize(rectPath);
assert(s.w === Math.round(100 * scale) && s.h === Math.round(20 * scale),
       `rect screenshot is the rect's size, got ${s.w}x${s.h}`);

// The content is the rect: red left half, blue right half.
if (typeof bro !== 'undefined' && bro.image && typeof bro.image.decodeOriented === 'function') {
    const img = bro.image.decodeOriented(s.bytes);
    const at = (x, y) => { const i = (y * img.width + x) * img.channels; return [img.pixels[i], img.pixels[i + 1], img.pixels[i + 2]]; };
    const l = at(Math.floor(img.width * 0.25), Math.floor(img.height / 2));
    const r = at(Math.floor(img.width * 0.75), Math.floor(img.height / 2));
    assert(l[0] > 240 && l[2] < 16, 'left of the rect is the red box: ' + l);
    assert(r[2] > 240 && r[0] < 16, 'right of the rect is the blue box: ' + r);
}
fs.unlinkSync(rectPath);

// A rect hanging off the frame is cut to it.
const edgePath = path.join(dir, 'bro_test_rect_edge_' + Date.now() + '.png');
screenshot(edgePath, -10, -10, 30, 30);
const e = pngSize(edgePath);
assert(e.w === Math.round(20 * scale) && e.h === Math.round(20 * scale),
       `a rect off the frame's corner is clipped to it, got ${e.w}x${e.h}`);
fs.unlinkSync(edgePath);

// Bad arguments throw rather than silently saving the whole frame.
let threw = false;
try { screenshot(path.join(dir, 'bro_test_rect_bad.png'), 0, 0); } catch (err) { threw = true; }
assert(threw, 'screenshot(path, x, y) without w and h throws');
threw = false;
try { screenshot(path.join(dir, 'bro_test_rect_bad.png'), 0, 0, 0, 10); } catch (err) { threw = true; }
assert(threw, 'screenshot with an empty rect throws');

console.log('screenshot rect OK');
