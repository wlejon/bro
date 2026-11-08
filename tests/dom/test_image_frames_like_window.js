// Headless loads images as a window does: a frame settles the decodes that
// have finished and paints the gap for the rest (layout/image_loading.h),
// while a read of the frame holds the pictures it paints. Verifies, with a
// large screenshot PNG (its decode takes a while) behind a fresh name:
//   - the src setter leaves the image incomplete (natural size from the
//     header) and no frame-free wait settles it (flush() only settles what
//     finished: it is still incomplete if the decode is still running)
//   - a capture right after the src setter paints the picture, not the gap
//   - while it decodes the settle state says `images`; waitForImages()
//     settles it (complete, load queued) and leaves nothing pending

const fs = require('fs');
const os = require('os');
const path = require('path');

document.body.style.margin = '0';
document.body.innerHTML =
    '<div style="width:1600px;height:1200px;background:linear-gradient(90deg,#f00,#00f)"></div>';
flush();
const SHOT = path.join(os.tmpdir(), 'bro_frames_like_window_' + Date.now() + '.png');
screenshot(SHOT);
document.body.innerHTML = '<div id="bg" style="position:absolute;left:0;top:0;width:200px;height:100px;background:#000"></div>';
flush();

const img = document.createElement('img');
img.style.cssText = 'position:absolute;left:0;top:0;width:200px;height:100px';
let loaded = false;
img.onload = () => { loaded = true; };
img.src = SHOT;
document.body.appendChild(img);
assert(img.complete === false, 'the src setter leaves the image decoding');
assert(img.naturalWidth > 0, 'its natural size comes from the header at once');
flush();
const pendingAfterFlush = perf.pendingWork().filter((r) => /^images/.test(r));
console.log(`after flush: complete ${img.complete}, pending ${JSON.stringify(pendingAfterFlush)}`);
if (!img.complete) assert(pendingAfterFlush.length > 0, 'a decode still running is pending work: images');

// The capture paints the picture (red at the left), not the black backdrop.
const p = getPixel(5, 50);
assert(p.r > 150 && p.b < 100, `the capture paints the decoding image (${p.r},${p.g},${p.b})`);

assert(waitForImages() === true, 'waitForImages leaves nothing decoding');
assert(img.complete === true, 'and the image is complete');
assert(perf.pendingWork().filter((r) => /^images/.test(r)).length === 0, 'nothing pending: images');
await new Promise((r) => setTimeout(r, 0));
assert(loaded, 'its load event fired');
try { fs.unlinkSync(SHOT); } catch (e) {}
console.log('test_image_frames_like_window: OK');
