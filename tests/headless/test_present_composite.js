// The frame composite with a full-viewport GPU layer, end to end.
//
// A 3D scene or WebGL canvas covering the whole viewport is not read back
// into the CPU composite: the presenter takes its GPU image as the frame's
// middle layer, with the UI painted before it (the canvas's own CSS
// background, the page) below and the UI painted after it above, blended in
// one GPU pass — and a capture reads that composite back. So:
//   - the scene shows over the canvas background (the layer below), not
//     under it;
//   - UI after the canvas blends over the scene (the layer above);
//   - where the scene draws nothing, the layer below shows through;
//   - once the GPU layer is gone, a capture is the CPU composite alone, not
//     a stale GPU frame.

document.body.style.margin = '0';
const W = window.innerWidth, H = window.innerHeight;

function close(px, r, g, b, tol, what) {
    const ok = Math.abs(px.r - r) <= tol && Math.abs(px.g - g) <= tol && Math.abs(px.b - b) <= tol;
    assert(ok, `${what}: expected ~(${r},${g},${b}), got (${px.r},${px.g},${px.b},${px.a})`);
}

// ── A full-viewport scene between two UI layers ─────────────────────────────
const canvas = document.createElement('canvas');
canvas.style.cssText = 'position:fixed;left:0;top:0;width:100vw;height:100vh;background:#203040';
document.body.appendChild(canvas);
const scn = canvas.getContext('scene');
if (!scn) missingGpuContext('scene');
else runWithScene();

function runWithScene() {
scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
scn.createLight({ type: 'directional', direction: [0, 0, -1], color: [1, 1, 1], intensity: 2 });
scn.createMesh({ mesh: 'box', color: 'red', x: 0, y: 0, z: 0 });
scn.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 5], target: [0, 0, 0] });

const ui = document.createElement('div');
ui.style.cssText = 'position:fixed;left:0;top:0;width:40px;height:40px;background:rgba(0,255,0,0.5)';
document.body.appendChild(ui);
const overBox = document.createElement('div');
overBox.style.cssText = `position:fixed;left:${(W >> 1) - 10}px;top:${(H >> 1) - 10}px;` +
                        'width:20px;height:20px;background:rgb(255,255,0)';
document.body.appendChild(overBox);
advanceTime(50);

const center = getPixel((W >> 1) + 30, H >> 1);
assert(center.r > 120 && center.g < 40 && center.b < 40,
    `scene box shows over the canvas background (got ${center.r},${center.g},${center.b})`);
close(getPixel(W - 5, H - 5), 0x20, 0x30, 0x40, 2, 'canvas background below the scene where it draws nothing');
close(getPixel(W >> 1, H >> 1), 255, 255, 0, 2, 'opaque UI above the scene');
// 50% green over the canvas background (the scene draws nothing in that corner).
close(getPixel(10, 10), 0x10, 0x98, 0x20, 3, 'half-transparent UI above, blended');

// getPixels reads one composite: the same answers in a block.
const block = getPixels((W >> 1) - 15, (H >> 1) - 2, 4, 4);
assert(block.width === 4 && block.height === 4, 'getPixels block size');
assert(block.data[0] > 120 && block.data[1] < 40, 'block corner is the scene');

// ── The GPU layer goes away: the capture is the CPU composite alone ─────────
canvas.remove();
advanceTime(50);
const after = getPixel((W >> 1) + 30, H >> 1);
assert(after.r === 255 && after.g === 255 && after.b === 255,
    `no stale scene frame once the canvas is gone (got ${after.r},${after.g},${after.b})`);
close(getPixel(W >> 1, H >> 1), 255, 255, 0, 2, 'UI still drawn without a GPU layer');

// ── A full-viewport WebGL canvas between UI layers ──────────────────────────
const glCanvas = document.createElement('canvas');
glCanvas.width = W;
glCanvas.height = H;
glCanvas.style.cssText = 'position:fixed;left:0;top:0;width:100vw;height:100vh';
document.body.insertBefore(glCanvas, ui);
const gl = glCanvas.getContext('webgl2');
assert(gl, 'webgl2 context');
function drawGL() {
    gl.clearColor(0.0, 0.0, 1.0, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    requestAnimationFrame(drawGL);
}
requestAnimationFrame(drawGL);
advanceTime(50);
close(getPixel(W - 5, H - 5), 0, 0, 255, 2, 'WebGL clear color as the frame layer');
close(getPixel(W >> 1, H >> 1), 255, 255, 0, 2, 'opaque UI above WebGL');
close(getPixel(10, 10), 0, 128, 128, 3, 'half-transparent UI blended over WebGL');
}
