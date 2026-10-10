// Taking delivery of a big ImageBitmap from a Worker costs the page no frame
// (docs/imagebitmap-api.js, "Transfer and the GPU").
//
// A 24 MP (6000 x 4000, 96 MB) bitmap made in a worker and transferred:
//  - the worker's handle is detached (0 x 0) once it is posted;
//  - the page receives it without copying its pixels: the delivery costs the
//    page thread well under the ~20 ms two 96 MB copies took;
//  - its texture upload starts when it arrives, off the page thread, so the
//    first frame that draws it does not pay the upload (mips included);
//  - it draws the right pixels whether the draw comes long after the upload
//    finished or in the very frame it arrived (the upload still in flight),
//    into a 2D canvas and through a bitmaprenderer canvas;
//  - a 24 MP <img> is uploaded when its decode lands, the same way.
//
// A "frame" here is flush() plus a getPixel(): layout, the canvas replay, the
// composite of the whole page and a readback, so the GPU's share of the work
// (an upload's copy) is in it too. Each timing is printed; the thresholds are
// generous (several times what a run takes) and still fail the old
// copy-on-receive, upload-on-draw path.

const fs = require('fs');
const path = require('path');
const os = require('os');

const W = 6000, H = 4000;
const RED = [255, 0, 0, 255], GREEN = [0, 255, 0, 255], BLUE = [0, 0, 255, 255], YELLOW = [255, 255, 0, 255];

const worker = new Worker('../image/worker_big_bitmap.js');
const inbox = [];
worker.onmessage = (e) => inbox.push(e.data);

// One advanceTime with nothing to deliver, for scale.
function tickMs() {
    const t = perf.now();
    advanceTime(16);
    return perf.now() - t;
}
let idleTick = Infinity;
for (let i = 0; i < 8; i++) idleTick = Math.min(idleTick, tickMs());

// Ask the worker for a bitmap; returns it and the page-thread cost of the
// tick that delivered it (net of an idle tick).
function requestBitmap(tag) {
    worker.postMessage({ w: W, h: H, tag });
    const deadline = Date.now() + 60000;
    while (Date.now() < deadline) {
        const before = inbox.length;
        const dt = tickMs();
        const got = inbox.slice(before).find((m) => m.tag === tag && m.bitmap);
        if (got) return { bmp: got.bitmap, recvMs: Math.max(0, dt - idleTick) };
        wallSleep(1);
    }
    throw new Error('the worker sent no bitmap for ' + tag);
}
function workerAfter(tag) {
    const deadline = Date.now() + 15000;
    let m;
    while (!(m = inbox.find((x) => x.tag === tag && x.after)) && Date.now() < deadline) {
        advanceTime(16);
        wallSleep(1);
    }
    return m && m.after;
}

function near(a, b) {
    return a.every((v, i) => Math.abs(v - b[i]) <= 2);
}
function rgbaOf(p) {
    return [p.r, p.g, p.b, p.a];
}
function checkQuadrants(read, cw, ch, what) {
    const probes = [
        [cw * 0.25, ch * 0.25, RED, 'top-left red'],
        [cw * 0.75, ch * 0.25, GREEN, 'top-right green'],
        [cw * 0.25, ch * 0.75, BLUE, 'bottom-left blue'],
        [cw * 0.75, ch * 0.75, YELLOW, 'bottom-right yellow'],
    ];
    for (const [x, y, want, name] of probes) {
        const got = read(Math.floor(x), Math.floor(y));
        assert(near(got, want), `${what}: ${name}, got ${got}`);
    }
}
const canvasPixel = (ctx) => (x, y) => Array.from(ctx.getImageData(x, y, 1, 1).data);
const pagePixel = (x, y) => rgbaOf(getPixel(x, y));

// The element under test is always the page's first box, 300 x 200 CSS px.
const BOX_W = 300, BOX_H = 200;
function frameMs() {
    const t = perf.now();
    flush();
    getPixel(BOX_W / 2, BOX_H / 2);
    return perf.now() - t;
}

document.body.style.margin = '0';
const CW = 1200, CH = 800;
function makeCanvas() {
    const c = document.createElement('canvas');
    c.width = CW;
    c.height = CH;
    c.style.cssText = `display:block;width:${BOX_W}px;height:${BOX_H}px`;
    document.body.appendChild(c);
    return c;
}
function idleFrameMs() {
    let best = Infinity;
    for (let i = 0; i < 4; i++) best = Math.min(best, frameMs());
    return best;
}

// --- A: receive, let the upload land, then draw -----------------------------
{
    const { bmp, recvMs } = requestBitmap('a');
    console.log(`receive 24 MP: ${recvMs.toFixed(2)} ms (idle tick ${idleTick.toFixed(2)} ms)`);
    assert(bmp instanceof ImageBitmap, 'the page got an ImageBitmap');
    assert(bmp.width === W && bmp.height === H, `the page's bitmap is ${W}x${H}, got ${bmp.width}x${bmp.height}`);
    const after = workerAfter('a');
    assert(after && after.width === 0 && after.height === 0,
           `the worker's bitmap is detached after the transfer, got ${JSON.stringify(after)}`);
    assert(recvMs < 8, `receiving a 24 MP bitmap costs the page under 8 ms, took ${recvMs.toFixed(2)} ms`);

    wallSleep(500);  // the upload, off the page thread
    advanceTime(16);
    const c = makeCanvas();
    const ctx = c.getContext('2d');
    const idle = idleFrameMs();
    ctx.drawImage(bmp, 0, 0, CW, CH);
    const drawMs = frameMs() - idle;
    console.log(`first frame drawing it into a canvas (upload landed): ${drawMs.toFixed(2)} ms over an idle frame of ${idle.toFixed(2)} ms`);
    checkQuadrants(canvasPixel(ctx), CW, CH, 'drawn after the upload');
    checkQuadrants(pagePixel, BOX_W, BOX_H, 'the canvas on the page');
    // A crop across the centre, at full resolution.
    ctx.drawImage(bmp, W / 2 - 10, H / 2 - 10, 20, 20, 0, 0, 20, 20);
    const px = canvasPixel(ctx);
    assert(near(px(5, 5), RED) && near(px(15, 5), GREEN) && near(px(5, 15), BLUE) && near(px(15, 15), YELLOW),
           'a full-resolution crop across the centre draws the four quadrants');
    assert(drawMs < 15, `the first frame drawing an uploaded 24 MP bitmap costs under 15 ms, took ${drawMs.toFixed(2)} ms`);
    bmp.close();
    assert(bmp.width === 0 && bmp.height === 0, 'close() detaches the page bitmap');
    c.remove();
}

// --- B: draw in the frame it arrives, the upload still in flight ------------
// Headless captures never defer a canvas (CanvasScene::rasterize), so this
// frame waits for the rest of the staging copy and its getPixel for the GPU
// copy: it checks the pixels are right, and prints what it took. (The
// windowed frame holds the canvas back a frame instead.)
{
    const c = makeCanvas();
    const ctx = c.getContext('2d');
    idleFrameMs();
    const { bmp } = requestBitmap('b');
    ctx.drawImage(bmp, 0, 0, CW, CH);
    const drawMs = frameMs();
    console.log(`first frame drawing it into a canvas on arrival: ${drawMs.toFixed(2)} ms`);
    checkQuadrants(canvasPixel(ctx), CW, CH, 'drawn on arrival');
    checkQuadrants(pagePixel, BOX_W, BOX_H, 'the canvas drawn on arrival, on the page');
    bmp.close();
    c.remove();
}

// --- C: shown through a bitmaprenderer canvas --------------------------------
{
    const { bmp } = requestBitmap('c');
    wallSleep(500);
    advanceTime(16);
    const c = document.createElement('canvas');
    c.style.cssText = `display:block;width:${BOX_W}px;height:${BOX_H}px`;
    document.body.appendChild(c);
    const brc = c.getContext('bitmaprenderer');
    const idle = idleFrameMs();
    brc.transferFromImageBitmap(bmp);
    const showMs = frameMs() - idle;
    console.log(`first frame showing it through bitmaprenderer: ${showMs.toFixed(2)} ms over an idle frame of ${idle.toFixed(2)} ms`);
    assert(bmp.width === 0, 'transferFromImageBitmap detaches the bitmap');
    checkQuadrants(pagePixel, BOX_W, BOX_H, 'shown through bitmaprenderer');
    c.remove();
    // What is left is the canvas's own 24 MP surface, made and filled on the
    // GPU (~7 ms); the 96 MB upload in the frame is gone.
    assert(showMs < 20, `showing a 24 MP bitmap through bitmaprenderer costs under 20 ms, took ${showMs.toFixed(2)} ms`);
}

// --- D: a 24 MP <img> --------------------------------------------------------
{
    // Quadrants again, as RGB, through a JPEG (flat colour, so it is small).
    const rgb = new Uint8Array(W * H * 3);
    const cols = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0]];
    const rowL = [new Uint8Array(W / 2 * 3), new Uint8Array(W / 2 * 3), new Uint8Array(W / 2 * 3), new Uint8Array(W / 2 * 3)];
    for (let q = 0; q < 4; q++) for (let i = 0; i < W / 2; i++) rowL[q].set(cols[q], i * 3);
    for (let y = 0; y < H; y++) {
        const top = y < H / 2;
        rgb.set(top ? rowL[0] : rowL[2], y * W * 3);
        rgb.set(top ? rowL[1] : rowL[3], y * W * 3 + W / 2 * 3);
    }
    const jpeg = bro.image.encodeJpeg(rgb, W, H, 3, 95);
    const dir = path.join(os.tmpdir(), 'bro_bigbitmap_' + process.pid + '_' + Date.now());
    fs.mkdirSync(dir, { recursive: true });
    const file = path.join(dir, 'big.jpg');
    fs.writeFileSync(file, jpeg);

    const img = document.createElement('img');
    img.style.cssText = `display:block;width:${BOX_W}px;height:${BOX_H}px`;
    img.style.visibility = 'hidden';
    document.body.appendChild(img);
    img.src = file;
    flush();  // waits for the decode
    wallSleep(500);  // the upload, off the page thread
    advanceTime(16);
    assert(img.complete && img.naturalWidth === W, `the <img> decoded at ${W} wide, got ${img.naturalWidth}`);
    const idle = idleFrameMs();
    img.style.visibility = 'visible';
    const showMs = frameMs() - idle;
    console.log(`first frame showing a 24 MP <img>: ${showMs.toFixed(2)} ms over an idle frame of ${idle.toFixed(2)} ms`);
    // JPEG edges ring a little: probe away from them.
    checkQuadrants((x, y) => {
        const p = pagePixel(x, y);
        return p.map((v) => (v > 240 ? 255 : v < 15 ? 0 : v));
    }, BOX_W, BOX_H, 'the <img>');
    assert(showMs < 15, `the first frame showing a decoded 24 MP <img> costs under 15 ms, took ${showMs.toFixed(2)} ms`);
    img.remove();
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}
}

worker.terminate();
