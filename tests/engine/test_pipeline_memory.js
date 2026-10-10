// Memory stays flat over thousands of frames of the windowed frame loop.
//
// runFrames() runs the frame a window runs (docs/headless.md, "The windowed
// pipeline"): the layout thread with its snapshot handoff, the raster thread
// replaying into double-buffered layer pools, the presenter, idle holds, the
// event pump. advanceTime() never touches most of that, so a leak there was
// invisible to every other headless test. This one builds an ordinary page
// (a list, a canvas, a worker), works it the way a user works an app — hover,
// wheel, a picture from the worker every few frames, drawn and closed — and
// idles it, and fails if the process keeps growing once it has warmed up.
//
// What it caught first: ImageBitmap.close() emptied the bitmap's pixel
// vector with clear(), which keeps the buffer, so every picture a page closed
// stayed allocated until its wrapper was collected — and a page that only
// draws pictures allocates too little JS for a collection to come round. An
// image viewer grew by a full-size picture per picture shown.

const MB = 1048576;
const mem = () => perf.stats().processBytes;

// ---- the page --------------------------------------------------------------
document.body.style.margin = '0';
const list = document.createElement('div');
list.style.cssText = 'height:400px;overflow:auto;font:14px sans-serif';
for (let i = 0; i < 300; i++) {
    const row = document.createElement('div');
    row.className = 'row';
    row.textContent = `Row ${i}: some text to shape and paint`;
    row.style.cssText = 'padding:4px 8px;border-bottom:1px solid #ccc';
    list.appendChild(row);
}
document.body.appendChild(list);
const style = document.createElement('style');
style.textContent = '.row:hover { background: #def; }';
document.head.appendChild(style);

const canvas = document.createElement('canvas');
canvas.width = 640;
canvas.height = 360;
document.body.appendChild(canvas);
const ctx = canvas.getContext('2d');

// ---- a picture at a time, from a worker -------------------------------------
const W = 1024, H = 768;
const worker = new Worker('../engine/pipeline_memory_worker.js');
let shown = null, received = 0, asked = 0;
worker.onmessage = (e) => {
    const bmp = e.data.bitmap;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(bmp, 0, 0, canvas.width, canvas.height);
    if (shown) shown.close();
    shown = bmp;
    received++;
};

const step = { stepMs: 16 };
function work(frames) {
    let presented = 0;
    for (let k = 0; k < frames; k += 4) {
        const i = k / 4;
        mouseMove(20 + (i * 37) % 600, 20 + (i * 53) % 380);
        if (i % 6 === 0) wheel(300, 200, (i / 6) % 10 < 5 ? 80 : -80);
        if (i % 4 === 0 && asked - received < 2) worker.postMessage({ w: W, h: H, v: 30 + (asked++ % 200) });
        presented += runFrames(4, step).presented;
    }
    return presented;
}
function idle(frames) {
    return runFrames(frames, step);
}

// Warm up: pools, pipelines, caches, the code cache, the worker.
for (let i = 0; i < 3; i++) { work(200); idle(100); }
const warm = mem();
const samples = [];
for (let i = 0; i < 8; i++) {
    const presented = work(250);
    const r = idle(150);
    samples.push(mem());
    if (i === 0) {
        assert(presented > 0, `frames in use were presented (${presented})`);
        assert(r.held > 0, `an idle page holds its frames (${r.held} of ${r.frames} held)`);
    }
}
const end = samples[samples.length - 1];
console.log(`pictures ${received}; after warm-up ${(warm / MB).toFixed(1)} MB, then ` +
            samples.map((s) => (s / MB).toFixed(0)).join(' ') + ' MB');

assert(received >= 40, `the worker's pictures arrived (${received})`);
// A picture is 3 MB: before the close() fix the run grew by ~100 of them.
// What it may grow by now is allocator and cache slack, not a trend.
const grew = (end - warm) / MB;
assert(grew < 48, `memory after warm-up grew ${grew.toFixed(1)} MB over 3200 frames (allowed 48)`);
// And not a steady climb: the second half grows no more than the slack.
const half = (end - samples[3]) / MB;
assert(half < 24, `memory still climbing in the second half (+${half.toFixed(1)} MB)`);

worker.terminate();
if (shown) shown.close();
