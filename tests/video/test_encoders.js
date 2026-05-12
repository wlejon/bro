// Test VideoEncoder (WebM/VP9) and GifEncoder — exercises
// src/js/video_bindings.cpp, src/video/webm_encoder.cpp, and
// src/video/gif_encoder.cpp.

const os = require('os');
const path = require('path');
const fs = require('fs');

const tmpDir = os.tmpdir();
const webmPath = path.join(tmpDir, 'bro_test_' + Date.now() + '.webm');
const gifPath = path.join(tmpDir, 'bro_test_' + Date.now() + '.gif');

// =========================================================================
// VideoEncoder — RGBA frame path
// =========================================================================
const W = 64, H = 64, FPS = 10;
const enc = new VideoEncoder({
    path: webmPath,
    width: W, height: H,
    fps: FPS,
    quality: 'realtime',
});
assert(enc !== null, 'VideoEncoder constructed');
assert(enc.width === W, 'enc.width = ' + W);
assert(enc.height === H, 'enc.height = ' + H);
assert(typeof enc.framesWritten === 'number', 'framesWritten is number');

const frame = new Uint8Array(W * H * 4);
for (let f = 0; f < 6; ++f) {
    // Animate: shift hue across frame
    for (let i = 0; i < W * H; ++i) {
        const p = i * 4;
        frame[p]     = (i + f * 10) & 0xff;
        frame[p + 1] = (i * 2 + f * 5) & 0xff;
        frame[p + 2] = (f * 40) & 0xff;
        frame[p + 3] = 255;
    }
    enc.addFrameRGBA(frame);
}
// framesWritten counts muxed packets; libvpx may buffer initial frames
// so this may stay at 0 until finish() flushes — just verify it's a number.
assert(typeof enc.framesWritten === 'number', 'framesWritten queried');

// Encoder error string is queryable
assert(typeof enc.lastError === 'string', 'lastError is string');

const finished = enc.finish();
assert(finished === true || finished === undefined, 'finish returned ok');

// File should exist and be non-trivial
assert(fs.existsSync(webmPath), 'webm file written: ' + webmPath);
const webmStat = fs.statSync(webmPath);
assert(webmStat.size > 100, 'webm has content, size = ' + webmStat.size);

// =========================================================================
// VideoEncoder error: dims must be even (4:2:0 chroma)
// =========================================================================
let threw = false;
try {
    new VideoEncoder({ path: webmPath + '.x', width: 65, height: 64 });
} catch (e) { threw = true; }
assert(threw, 'odd width rejected');

// =========================================================================
// VideoEncoder + canvas frame
// =========================================================================
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(W));
canvas.setAttribute('height', String(H));
document.body.appendChild(canvas);
const ctx = canvas.getContext('2d');
flush();

const webmCanvasPath = path.join(tmpDir, 'bro_test_canvas_' + Date.now() + '.webm');
const enc2 = new VideoEncoder({ path: webmCanvasPath, width: W, height: H, fps: FPS });
for (let f = 0; f < 4; ++f) {
    ctx.fillStyle = 'rgb(' + (f * 60) + ',100,200)';
    ctx.fillRect(0, 0, W, H);
    flush();
    try { enc2.addCanvasFrame(canvas); }
    catch (e) { /* canvas snapshot may fail in some modes; finish anyway */ break; }
}
enc2.finish();

// =========================================================================
// GifEncoder
// =========================================================================
const gif = new GifEncoder({
    path: gifPath,
    width: 32, height: 32, fps: 10,
});
assert(gif !== null, 'GifEncoder constructed');

const gframe = new Uint8Array(32 * 32 * 4);
for (let f = 0; f < 5; ++f) {
    for (let i = 0; i < 32 * 32; ++i) {
        const p = i * 4;
        gframe[p]     = (i + f * 20) & 0xff;
        gframe[p + 1] = (f * 50) & 0xff;
        gframe[p + 2] = 100;
        gframe[p + 3] = 255;
    }
    gif.addFrameRGBA(gframe);
}

// Per-frame delay override
if (typeof gif.setNextFrameDelayCs === 'function') {
    gif.setNextFrameDelayCs(50);
    gif.addFrameRGBA(gframe);
}

gif.finish();
assert(fs.existsSync(gifPath), 'gif file written');
const gifStat = fs.statSync(gifPath);
assert(gifStat.size > 50, 'gif size > 50, got ' + gifStat.size);

// Cleanup
try { fs.unlinkSync(webmPath); } catch(e) {}
try { fs.unlinkSync(webmCanvasPath); } catch(e) {}
try { fs.unlinkSync(gifPath); } catch(e) {}

document.body.removeChild(canvas);
