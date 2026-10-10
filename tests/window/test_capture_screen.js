// bro.window.captureScreen(path): a PNG of what is on screen. Under DRM it
// reads the KMS scanout buffer (client windows + shell, as composited) — that
// half can only be checked on a live DRM session; headless it is the engine's
// own composite, which this checks: the file is written, at the viewport size,
// with the page's pixels in it.
const fs = require('fs');
const os = require('os');
const path = require('path');

assert(typeof bro.window.captureScreen === 'function', 'bro.window.captureScreen exists');

document.body.style.margin = '0';
document.body.innerHTML =
    '<div style="position:absolute;left:0;top:0;width:100px;height:100px;background:rgb(255,0,0)"></div>';
flush();

const out = path.join(os.tmpdir(), 'bro_capture_screen_' + Date.now(), 'shot.png');
const written = bro.window.captureScreen(out);
assert(written === out, 'captureScreen returns the path it wrote, got ' + written);
assert(fs.existsSync(out), 'the PNG exists');

const png = fs.readFileSync(out);
assert(png[0] === 0x89 && png[1] === 0x50 && png[2] === 0x4e && png[3] === 0x47, 'it is a PNG');
const be32 = (o) => ((png[o] << 24) | (png[o + 1] << 16) | (png[o + 2] << 8) | png[o + 3]) >>> 0;
const w = be32(16), h = be32(20);
assert(w === window.innerWidth * devicePixelRatio && h === window.innerHeight * devicePixelRatio,
       'the PNG is the viewport size, got ' + w + 'x' + h);

// The pixels are the page's: decode it back and look at the red square.
const img = new Image();
img.src = out;
flush();  // settles the off-thread decode
const c = document.createElement('canvas');
c.width = 20; c.height = 20;
const ctx = c.getContext('2d');
ctx.drawImage(img, 0, 0);
const px = ctx.getImageData(10, 10, 1, 1).data;
assert(px[0] > 200 && px[1] < 50 && px[2] < 50, 'the captured frame shows the page, got ' + Array.from(px));

fs.rmSync(path.dirname(out), { recursive: true, force: true });
console.log('captureScreen OK');
