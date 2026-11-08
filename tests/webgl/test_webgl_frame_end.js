// A WebGL canvas's frame end in the live loop does not wait for the WebGL
// thread to finish replaying the frame: the script's next frame runs while
// it does, and the compositor samples the canvas's front, which holds the
// frame just ended or, when the replay has not reached its end yet, the one
// before it (never older: the loop runs at most one frame ahead). Everything
// that reads the canvas still sees the frame just drawn: readPixels in the
// frame, a drawImage of the canvas, and a capture (getPixel, frameImage),
// whose composite waits for the frame's end.
//
// The loop's frames upload a few megabytes of texture each, so the WebGL
// thread's replay of a frame is long beside the script that encodes it.

document.body.style.margin = '0';
const W = 64, H = 64;
const c = document.createElement('canvas');
c.width = W; c.height = H;
c.style.cssText = `position:absolute; left:0; top:0; width:${W}px; height:${H}px;`;
document.body.appendChild(c);
flush();

const gl = c.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const colour = (k) => [(k * 37 + 11) & 255, (k * 91 + 50) & 255, 200];
    const near = (p, rgb) => Math.abs(p[0] - rgb[0]) <= 2 && Math.abs(p[1] - rgb[1]) <= 2 && Math.abs(p[2] - rgb[2]) <= 2;
    const glPixel = () => {
        const b = new Uint8Array(4);
        gl.readPixels(W >> 1, H >> 1, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Array.from(b);
    };
    // createImageBitmap of the WebGL canvas: its drawing buffer, read back
    // when called; the bitmap's pixels are checked at the end.
    const copy = document.createElement('canvas');
    copy.width = W; copy.height = H;
    const copyCtx = copy.getContext('2d');
    const snapshots = [];
    const snapshot = (expected, label) => snapshots.push({ bitmap: createImageBitmap(c), expected, label });
    async function checkSnapshots() {
        for (const { bitmap, expected, label } of snapshots) {
            const bmp = await bitmap;
            copyCtx.clearRect(0, 0, W, H);
            copyCtx.drawImage(bmp, 0, 0);
            const d = Array.from(copyCtx.getImageData(W >> 1, H >> 1, 1, 1).data);
            assert(near(d, expected), `${label}: createImageBitmap of the canvas reads the frame drawn, got ${d}`);
        }
    }

    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    const upload = new Uint8Array(512 * 512 * 4);
    let drawn = -1;
    let inFrameChecks = 0;
    function draw(k) {
        // The heavy part of the replay: texture uploads.
        for (let u = 0; u < 6; u++) {
            upload[u] = k & 255;
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 512, 512, 0, gl.RGBA, gl.UNSIGNED_BYTE, upload);
        }
        const rgb = colour(k);
        gl.clearColor(rgb[0] / 255, rgb[1] / 255, rgb[2] / 255, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        drawn = k;
    }

    // --- Headless steps: each composite shows what its script drew -----------
    for (let k = 0; k < 12; k++) {
        draw(k);
        if (k % 3 === 0) {
            const p = glPixel();
            assert(near(p, colour(k)), `step ${k}: readPixels reads the frame drawn, got ${p}`);
        }
        if (k % 4 === 1) snapshot(colour(k), `step ${k}`);
        flush();
        const s = getPixel(W >> 1, H >> 1);
        assert(near([s.r, s.g, s.b], colour(k)), `step ${k}: the step shows the frame drawn, got ${s.r},${s.g},${s.b}`);
    }

    // --- The live loop: the frame shown is this one or the one before --------
    runFrames(1, { stepMs: 16 });
    let frame = 100;
    let resized = false;
    function tick() {
        draw(frame);
        if (frame % 3 === 0 && !resized) {
            const p = glPixel();
            assert(near(p, colour(frame)), `frame ${frame}: readPixels in the frame reads it, got ${p}`);
            inFrameChecks++;
        }
        if (frame % 4 === 2 && !resized) snapshot(colour(frame), `frame ${frame}`);
        frame++;
        requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);

    let lagged = 0, current = 0;
    let before = drawn;  // the last frame drawn before this pass
    for (let i = 0; i < 40; i++) {
        runFrames(1, { stepMs: 16 });
        const k = drawn;
        assert(k >= 100, `the loop drew (drawn ${k})`);
        const img = presentedImage();
        assert(img, 'the loop presented a frame');
        const at = ((H >> 1) * img.width + (W >> 1)) * 4;
        const p = [img.data[at], img.data[at + 1], img.data[at + 2]];
        if (near(p, colour(k))) current++;
        else if (near(p, colour(before))) lagged++;
        else assert(false, `loop frame ${k}: presented ${p}, neither it (${colour(k)}) nor the one before (${colour(before)})`);
        before = k;

        // A capture composites what the script drew last, whatever the loop showed.
        if (i % 4 === 0) {
            const s = getPixel(W >> 1, H >> 1);
            assert(near([s.r, s.g, s.b], colour(k)), `loop frame ${k}: getPixel shows it, got ${s.r},${s.g},${s.b}`);
        }
        if (i % 5 === 2) {
            const f = frameImage();
            const q = [f.data[at], f.data[at + 1], f.data[at + 2]];
            assert(near(q, colour(k)), `loop frame ${k}: frameImage shows it, got ${q}`);
        }
    }
    assert(inFrameChecks > 5, `readPixels checked in the loop's frames (${inFrameChecks})`);

    // A resize: the first frame end after it shows the new drawing buffer.
    resized = true;
    c.width = W; c.height = H >> 1;
    runFrames(2, { stepMs: 16 });
    const s = getPixel(W >> 1, H >> 2);
    assert(near([s.r, s.g, s.b], colour(drawn)), `after a resize: the frame drawn shows, got ${s.r},${s.g},${s.b}`);
    assert(gl.getError() === gl.NO_ERROR, 'no GL error');
    assert(snapshots.length > 8, `snapshots taken (${snapshots.length})`);
    await checkSnapshots();
    console.log(`test_webgl_frame_end PASSED (loop frames shown current ${current}, one behind ${lagged})`);
}
