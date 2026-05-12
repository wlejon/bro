// Test the 2D-canvas streaming putImageData fast path. When a frame's
// canvas command buffer consists only of putImageData ops, CanvasScene
// short-circuits the SkCanvas replay and writes pixels directly to the
// surface via SkSurface::writePixels. We can't observe that bypass from
// JS, but we CAN verify the visible result is correct — same as the
// regular Skia replay would produce.

const W = 16, H = 16;
document.getElementById('root').innerHTML =
    `<canvas id="cv" width="${W}" height="${H}" style="width:${W}px;height:${H}px"></canvas>`;
flush();

const cv = document.getElementById('cv');
const ctx = cv.getContext('2d');

// ----- Single full-canvas putImageData (the streaming pattern) -----
const img = ctx.createImageData(W, H);
for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4;
        img.data[i + 0] = x * 16;       // R ramp across X
        img.data[i + 1] = y * 16;       // G ramp across Y
        img.data[i + 2] = 0;
        img.data[i + 3] = 255;
    }
}
ctx.putImageData(img, 0, 0);
flush();

let snap = ctx.getImageData(0, 0, W, H);
function pix(x, y) {
    const i = (y * W + x) * 4;
    return [snap.data[i], snap.data[i+1], snap.data[i+2], snap.data[i+3]];
}
assertArrayEqual(pix(0, 0), [0, 0, 0, 255], 'TL (0,0)');
assertArrayEqual(pix(W-1, 0), [(W-1)*16, 0, 0, 255], 'TR row');
assertArrayEqual(pix(0, H-1), [0, (H-1)*16, 0, 255], 'BL col');
assertArrayEqual(pix(W-1, H-1), [(W-1)*16, (H-1)*16, 0, 255], 'BR');

// ----- Coalesced: two putImageData calls in one frame; only last wins -----
// First call writes red; second writes blue. With the fast path keeping only
// the last call, the surface ends up blue. The regular Skia replay would
// produce the same result since putImageData uses kSrc blend mode.
const red  = ctx.createImageData(W, H);
const blue = ctx.createImageData(W, H);
for (let i = 0; i < W * H; i++) {
    red.data[i*4 + 0] = 255; red.data[i*4 + 3] = 255;
    blue.data[i*4 + 2] = 255; blue.data[i*4 + 3] = 255;
}
ctx.putImageData(red, 0, 0);
ctx.putImageData(blue, 0, 0);
flush();
snap = ctx.getImageData(0, 0, W, H);
assertArrayEqual([snap.data[0], snap.data[1], snap.data[2], snap.data[3]],
                 [0, 0, 255, 255], 'coalesced: blue wins');

// ----- Mixed: fillRect + putImageData must NOT trigger fast path -----
// (We can't directly observe that, but the result must still be correct.)
ctx.fillStyle = 'rgb(0,255,0)';
ctx.fillRect(0, 0, W, H);
ctx.putImageData(red, 0, 0);
flush();
snap = ctx.getImageData(0, 0, W, H);
assertArrayEqual([snap.data[0], snap.data[1], snap.data[2], snap.data[3]],
                 [255, 0, 0, 255], 'mixed ops: red putImageData wins');

// ----- Partial-region putImageData -----
// Write a 4×4 cyan tile at (8,8) into an otherwise-zeroed surface (cleared
// by the next createImageData snapshot below isn't true — we need an explicit
// pre-clear). Use a clearRect first.
ctx.clearRect(0, 0, W, H);
const cyan = ctx.createImageData(4, 4);
for (let i = 0; i < 16; i++) {
    cyan.data[i*4 + 1] = 255; cyan.data[i*4 + 2] = 255; cyan.data[i*4 + 3] = 255;
}
ctx.putImageData(cyan, 8, 8);
flush();
snap = ctx.getImageData(0, 0, W, H);
assertArrayEqual(pix(8, 8), [0, 255, 255, 255], 'partial: cyan tile at (8,8)');
assertArrayEqual(pix(0, 0), [0, 0, 0, 0],       'partial: untouched stays cleared');

document.getElementById('root').innerHTML = '';

function assertArrayEqual(a, b, msg) {
    if (a.length !== b.length) throw new Error(msg + ': len mismatch ' + a + ' vs ' + b);
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) throw new Error(msg + ': [' + i + '] got ' + a[i] + ' expected ' + b[i] + ' (full: ' + a + ' vs ' + b + ')');
    }
}
