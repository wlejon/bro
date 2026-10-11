// GifEncoder and VideoEncoder in a Worker: a worker renders frames and encodes
// them itself from typed arrays, so the page never holds the frames. The files
// it writes read back with the frame count and the per-frame delays asked for;
// the DOM-bound sources (a canvas, the viewport) are refused there.

if (typeof VideoEncoder === 'undefined') skipTest('this build has no video encoders (BRO_WITH_VIDEO)');

const fs = require('fs');
const os = require('os');
const path = require('path');

const dir = path.join(os.tmpdir(), `bro-enc-worker-${process.pid}-${Date.now()}`);
fs.mkdirSync(dir, { recursive: true });
const gifPath = path.join(dir, 'w.gif');
const webmPath = path.join(dir, 'w.webm');
const delays = [10, 25, 4, 50, 7];

let got = null;
const w = new Worker('../video/worker_encoders.js');
w.onmessage = (e) => { got = e.data; };
w.postMessage({ gifPath, webmPath, w: 32, h: 24, frames: delays.length, delays });
for (let i = 0; i < 1000 && !got; i++) { advanceTime(16); sleep(10); }
assert(got, 'the worker answered');
assert(!got.error, `the worker encoded: ${got.error}`);
assert(got.gifType === 'function' && got.webmType === 'function', `both encoders exist in a worker (${got.gifType}, ${got.webmType})`);
assert(got.gifInstance, 'an encoder made in a worker is an instance of its class there');
assert(got.gifFinished && got.webmFinished, 'both finished');
assert(got.webmFrames === delays.length, `the WebM muxed every frame (${got.webmFrames})`);
assert(/^TypeError: .*addFrameRGBA/.test(got.viewport), `addViewportFrame is refused in a worker: ${got.viewport}`);
assert(/^TypeError: .*addFrameRGBA/.test(got.canvas), `addCanvasFrame is refused in a worker: ${got.canvas}`);

const dec = bro.image.openFrames(gifPath);
assert(dec, 'the GIF decodes');
assert(dec.frameCount === delays.length, `GIF frame count ${dec.frameCount}`);
assert(JSON.stringify(dec.delays) === JSON.stringify(delays.map((cs) => cs * 10)), `GIF delays ${JSON.stringify(dec.delays)}`);
assert(dec.width === 32 && dec.height === 24, 'GIF size');
dec.close();

assert(fs.statSync(webmPath).size > 100, 'the WebM file has content');
// Read back through <video>: its frames, stepped one picture at a time.
const v = document.createElement('video');
v.muted = true;
document.body.appendChild(v);
v.src = webmPath;
flush();
assert(v.readyState >= 1, `the WebM loads (readyState ${v.readyState}, ${v.error && v.error.message})`);
assert(Math.abs(v.duration - delays.length / 10) < 0.011, `WebM duration ${v.duration}`);
let pictures = 1;
while (pictures < 100 && v.stepFrame(1) > 0) pictures++;
assert(pictures === delays.length, `WebM pictures stepped through: ${pictures}`);
v.remove();

w.terminate();
fs.rmSync(dir, { recursive: true, force: true });
console.log('test_encoders_worker PASSED');
