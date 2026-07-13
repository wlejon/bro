// Test ImageBitmap / createImageBitmap — the pixels-to-drawable primitive.
// Exercises src/js/imagebitmap_bindings.cpp: createImageBitmap from an
// ImageData-shaped source, an HTMLCanvasElement, an Image, and another
// ImageBitmap (shared vs. cropped copy); drawImage consumption (3/5/9-arg
// forms); WebGL texImage2D consumption; close(); and zero-copy Worker
// transfer (src/js/message_serializer.cpp). See docs/imagebitmap-api.js.

// --- helpers -------------------------------------------------------------
// A WxH RGBA buffer split into four solid-colour quadrants so crops/scales
// can be verified by sampling a single pixel per quadrant.
//   top-left = red, top-right = green, bottom-left = blue, bottom-right = yellow
function makeQuadrantRGBA(w, h) {
    const data = new Uint8ClampedArray(w * h * 4);
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = (y * w + x) * 4;
            const right = x >= w / 2;
            const bottom = y >= h / 2;
            if (!right && !bottom)      { data[i] = 255; data[i+1] = 0;   data[i+2] = 0;   data[i+3] = 255; }
            else if (right && !bottom) { data[i] = 0;   data[i+1] = 255; data[i+2] = 0;   data[i+3] = 255; }
            else if (!right && bottom) { data[i] = 0;   data[i+1] = 0;   data[i+2] = 255; data[i+3] = 255; }
            else                        { data[i] = 255; data[i+1] = 255; data[i+2] = 0;   data[i+3] = 255; }
        }
    }
    return data;
}

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();
const ctx = canvas.getContext('2d');
assert(ctx !== null && ctx !== undefined, 'getContext("2d") returns a context');

const W = 4, H = 4;
const rgba = makeQuadrantRGBA(W, H);

// =========================================================================
// createImageBitmap from an ImageData-shaped source — the load-bearing case
// =========================================================================
const bmp = await createImageBitmap({ width: W, height: H, data: rgba });
assert(bmp !== null && bmp !== undefined, 'createImageBitmap resolves');
assert(bmp.width === W, 'bitmap width = ' + W + ', got ' + bmp.width);
assert(bmp.height === H, 'bitmap height = ' + H + ', got ' + bmp.height);

// --- drawImage(bitmap, dx, dy) — native size ---
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(bmp, 0, 0);
let px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 255 && px[1] === 0 && px[2] === 0, 'drawImage native: top-left red, got ' + Array.from(px));
px = ctx.getImageData(W - 1, 0, 1, 1).data;
assert(px[0] === 0 && px[1] === 255 && px[2] === 0, 'drawImage native: top-right green, got ' + Array.from(px));
px = ctx.getImageData(0, H - 1, 1, 1).data;
assert(px[0] === 0 && px[1] === 0 && px[2] === 255, 'drawImage native: bottom-left blue, got ' + Array.from(px));
px = ctx.getImageData(W - 1, H - 1, 1, 1).data;
assert(px[0] === 255 && px[1] === 255 && px[2] === 0, 'drawImage native: bottom-right yellow, got ' + Array.from(px));

// --- drawImage(bitmap, dx, dy, dw, dh) — scaled blit ---
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(bmp, 0, 0, 32, 32);
px = ctx.getImageData(30, 30, 1, 1).data; // near the scaled bottom-right corner
assert(px[0] === 255 && px[1] === 255 && px[2] === 0, 'drawImage scaled: bottom-right yellow, got ' + Array.from(px));

// --- drawImage(bitmap, sx,sy,sw,sh, dx,dy,dw,dh) — 9-arg crop+scale ---
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(bmp, 2, 0, 2, 2, 0, 0, 20, 20); // crop top-right quadrant
px = ctx.getImageData(5, 5, 1, 1).data;
assert(px[0] === 0 && px[1] === 255 && px[2] === 0, '9-arg drawImage crops top-right quadrant -> green, got ' + Array.from(px));

// =========================================================================
// createImageBitmap(source, sx, sy, sw, sh) — crop rect at creation time
// =========================================================================
const cropped = await createImageBitmap({ width: W, height: H, data: rgba }, 2, 2, 2, 2); // bottom-right quadrant
assert(cropped.width === 2 && cropped.height === 2,
       'cropped bitmap size = 2x2, got ' + cropped.width + 'x' + cropped.height);
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(cropped, 0, 0);
px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 255 && px[1] === 255 && px[2] === 0, 'crop-at-creation -> yellow quadrant, got ' + Array.from(px));

// =========================================================================
// createImageBitmap from another ImageBitmap (uncropped shares; cropped copies)
// =========================================================================
const shared = await createImageBitmap(bmp); // uncropped
assert(shared.width === W && shared.height === H, 'uncropped bitmap-from-bitmap keeps size');
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(shared, 0, 0);
px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 255 && px[1] === 0 && px[2] === 0, 'bitmap-from-bitmap draws same pixels, got ' + Array.from(px));

const croppedFromBmp = await createImageBitmap(bmp, 2, 0, 2, 2); // top-right quadrant, copies
assert(croppedFromBmp.width === 2 && croppedFromBmp.height === 2, 'cropped bitmap-from-bitmap size');
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(croppedFromBmp, 0, 0);
px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 0 && px[1] === 255 && px[2] === 0, 'cropped bitmap-from-bitmap -> green quadrant, got ' + Array.from(px));

// =========================================================================
// createImageBitmap from an HTMLCanvasElement
// =========================================================================
const srcCanvas = document.createElement('canvas');
srcCanvas.setAttribute('width', '8');
srcCanvas.setAttribute('height', '8');
document.body.appendChild(srcCanvas);
flush();
const srcCtx = srcCanvas.getContext('2d');
srcCtx.fillStyle = '#ff8000';
srcCtx.fillRect(0, 0, 8, 8);
const fromCanvas = await createImageBitmap(srcCanvas);
assert(fromCanvas.width === 8 && fromCanvas.height === 8,
       'bitmap-from-canvas size = 8x8, got ' + fromCanvas.width + 'x' + fromCanvas.height);
ctx.clearRect(0, 0, 64, 64);
ctx.drawImage(fromCanvas, 0, 0);
px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 255 && px[1] === 128 && px[2] === 0, 'bitmap-from-canvas draws orange fill, got ' + Array.from(px));
document.body.removeChild(srcCanvas);

// =========================================================================
// createImageBitmap from an Image (HTMLImageElement)
// =========================================================================
// screenshot() gives us a real PNG on disk without checking a binary asset
// into the tree. Image decoding is synchronous (stb_image), so the image is
// complete before createImageBitmap sees it.
const os = require('os');
const path = require('path');
const shotPath = path.join(os.tmpdir(), 'bro_test_bitmap_src_' + Date.now() + '.png');
screenshot(shotPath);

const img = new Image();
img.src = shotPath;
assert(img.complete === true, 'Image loads synchronously');
assert(img.naturalWidth > 0, 'screenshot decoded into the Image');
const fromImage = await createImageBitmap(img);
assert(fromImage.width === img.width && fromImage.height === img.height,
       'bitmap-from-Image matches natural size');

// A broken image is not a valid CanvasImageSource — it has no pixels, so
// createImageBitmap must reject rather than hand back a 1x1 white bitmap.
const brokenImg = new Image();
brokenImg.src = '/nonexistent-imagebitmap-source.png';
assert(brokenImg.complete === true, 'broken image still settles');
assert(brokenImg.naturalWidth === 0, 'broken image has no natural size');
let brokenRejected = false;
try {
    await createImageBitmap(brokenImg);
} catch (e) {
    brokenRejected = true;
}
assert(brokenRejected === true, 'createImageBitmap rejects a broken image');

// =========================================================================
// Malformed source rejects the promise
// =========================================================================
let rejected = false;
try {
    await createImageBitmap({ width: 0, height: 0, data: new Uint8ClampedArray(0) });
} catch (e) {
    rejected = true;
}
assert(rejected === true, 'malformed source rejects the promise');

// =========================================================================
// close() — releases the backing image eagerly; further draws are a no-op
// =========================================================================
const closable = await createImageBitmap({ width: W, height: H, data: rgba });
assert(closable.width === W, 'closable bitmap has size before close');
closable.close();
assert(closable.width === 0, 'width = 0 after close');
assert(closable.height === 0, 'height = 0 after close');

ctx.clearRect(0, 0, 64, 64);
ctx.fillStyle = '#123456';
ctx.fillRect(0, 0, 4, 4);
ctx.drawImage(closable, 0, 0); // no-op — must not throw, must not alter the canvas
px = ctx.getImageData(0, 0, 1, 1).data;
assert(px[0] === 0x12 && px[1] === 0x34 && px[2] === 0x56,
       'drawImage(closed bitmap) is a no-op, got ' + Array.from(px));

// =========================================================================
// WebGL texImage2D consumption (GPU headless mode only)
// =========================================================================
const glCanvas = document.createElement('canvas');
glCanvas.setAttribute('width', '4');
glCanvas.setAttribute('height', '4');
document.body.appendChild(glCanvas);
flush();
const gl = glCanvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping texImage2D(ImageBitmap) check');
} else {
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    const glBmp = await createImageBitmap({ width: W, height: H, data: rgba });
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, glBmp);
    assert(gl.getError() === gl.NO_ERROR, 'texImage2D(ImageBitmap) does not raise a GL error');
    gl.deleteTexture(tex);
}
document.body.removeChild(glCanvas);

// =========================================================================
// Worker transfer — zero-copy, source neutered (src/js/message_serializer.cpp)
// =========================================================================
const w1 = new Worker('../canvas/worker_imagebitmap.js');
let workerReply = null;
w1.onmessage = (e) => { workerReply = e.data; };

const transferBmp = await createImageBitmap({ width: W, height: H, data: rgba });
assert(transferBmp.width === W, 'transfer source has size before postMessage');
w1.postMessage({ cmd: 'dims', bitmap: transferBmp }, [transferBmp]);
// Listed in the transfer list -> neutered synchronously on the source thread,
// same as ArrayBuffer transfer.
assert(transferBmp.width === 0 && transferBmp.height === 0,
       'transferred bitmap neutered on source thread');

let waited = 0;
while (workerReply === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(workerReply !== null, 'worker replied after receiving transferred bitmap');
assert(workerReply.width === W && workerReply.height === H,
       'worker saw correct transferred dimensions, got ' + JSON.stringify(workerReply));
w1.terminate();

// Not listed in the transfer list -> structured-cloned; since the image is
// immutable this shares the backing pixels rather than copying, and the
// source stays live.
const w2 = new Worker('../canvas/worker_imagebitmap.js');
let workerReply2 = null;
w2.onmessage = (e) => { workerReply2 = e.data; };
const cloneBmp = await createImageBitmap({ width: W, height: H, data: rgba });
w2.postMessage({ cmd: 'dims', bitmap: cloneBmp }); // no transfer list
assert(cloneBmp.width === W && cloneBmp.height === H, 'non-transferred bitmap stays live on source thread');

waited = 0;
while (workerReply2 === null && waited < 5000) { advanceTime(16); waited += 16; }
assert(workerReply2 !== null, 'worker replied for cloned bitmap');
assert(workerReply2.width === W && workerReply2.height === H, 'worker saw correct cloned dimensions');
w2.terminate();

// --- Cleanup ---
document.body.removeChild(canvas);
