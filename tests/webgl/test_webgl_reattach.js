// A WebGL canvas taken out of the document and put back keeps its context,
// its state and its drawing buffer, as in a browser: removing an element
// never destroys it, and a context lives as long as its canvas.
//
// Regression: the frame's sweep (the windowed loop, the headless step, the
// DRM loop) dropped the context of a canvas found out of the document. The
// canvas's gl object went on pointing at the freed context, and once the
// canvas was back it showed nothing. A virtual list recycling its cells
// removes a canvas and appends it again a frame or more later; the 2D half of
// this is test_canvas_reattach. Checked through the headless step and through
// the windowed frame loop, which sweeps on its own path.

document.body.style.margin = '0';

function makeCanvas(x) {
    const c = document.createElement('canvas');
    c.width = 32; c.height = 32;
    c.style.cssText = `position:absolute; left:${x}px; top:0; width:32px; height:32px;`;
    document.body.appendChild(c);
    return c;
}

const probe = makeCanvas(200).getContext('webgl2');
if (!probe) {
    missingGpuContext('webgl2');
} else {
    const near = (p, rgb) => [p.r, p.g, p.b].every((v, i) => Math.abs(v - rgb[i]) <= 3);
    const RED = [255, 0, 0], BLUE = [0, 0, 255], GREEN = [0, 255, 0];

    const stepRead = (x, y) => getPixel(x, y);
    function windowRead(x, y) {
        const img = presentedImage();
        assert(img, 'the frame loop presented a frame');
        const s = img.width / window.innerWidth;
        const i = (Math.floor(y * s) * img.width + Math.floor(x * s)) * 4;
        return { r: img.data[i], g: img.data[i + 1], b: img.data[i + 2] };
    }
    function check(where, x, rgb, read) {
        const p = read(x + 16, 16);
        assert(near(p, rgb), `${where}: expected rgb(${rgb}) at x=${x + 16}, got rgb(${p.r}, ${p.g}, ${p.b})`);
    }
    function fill(gl, rgb) {
        gl.clearColor(rgb[0] / 255, rgb[1] / 255, rgb[2] / 255, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }
    function readBack(gl) {
        const px = new Uint8Array(4);
        gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        return { r: px[0], g: px[1], b: px[2] };
    }

    function run(name, frames, read, x) {
        const c = makeCanvas(x);
        const gl = c.getContext('webgl2', { preserveDrawingBuffer: true });
        assert(gl, `${name}: a context`);
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([1, 2, 3, 4]));
        const buf = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buf);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([1, 2, 3]), gl.STATIC_DRAW);
        gl.enable(gl.SCISSOR_TEST);
        gl.scissor(0, 0, 32, 32);
        fill(gl, RED);
        frames();
        check(`${name}: drawn`, x, RED, read);

        // Out of the document for frames (the sweep runs), then back.
        c.remove();
        frames();
        frames();
        document.body.appendChild(c);
        frames();
        check(`${name}: back in the document, its drawing buffer kept`, x, RED, read);
        assert(!gl.isContextLost(), `${name}: the context is not lost`);
        assert(c.getContext('webgl2') === gl, `${name}: getContext answers the same context`);
        const cc = gl.getParameter(gl.COLOR_CLEAR_VALUE);
        assert(cc[0] === 1 && cc[1] === 0 && cc[2] === 0, `${name}: the clear colour is kept (${Array.from(cc)})`);
        assert(gl.isEnabled(gl.SCISSOR_TEST), `${name}: the scissor test stays on`);
        assert(gl.getParameter(gl.TEXTURE_BINDING_2D) === tex && gl.isTexture(tex), `${name}: the texture is kept and bound`);
        assert(gl.getParameter(gl.ARRAY_BUFFER_BINDING) === buf && gl.isBuffer(buf), `${name}: the buffer is kept and bound`);
        assert(gl.getBufferParameter(gl.ARRAY_BUFFER, gl.BUFFER_SIZE) === 12, `${name}: with its data`);
        assert(near(readBack(gl), RED), `${name}: readPixels reads the red`);
        assert(gl.getError() === gl.NO_ERROR, `${name}: no GL error`);

        // It still draws.
        fill(gl, BLUE);
        frames();
        check(`${name}: drawn after coming back`, x, BLUE, read);

        // Drawn while out of the document: shown once it is back.
        c.remove();
        frames();
        fill(gl, GREEN);
        frames();
        assert(near(readBack(gl), GREEN), `${name}: drawn while out, readPixels reads it`);
        document.body.appendChild(c);
        frames();
        check(`${name}: drawn while out, shown when back`, x, GREEN, read);
        assert(gl.getError() === gl.NO_ERROR, `${name}: no GL error at the end`);
    }

    run('step', () => { flush(); advanceTime(16); flush(); }, stepRead, 0);
    run('window', () => { runFrames(6, { stepMs: 16 }); }, windowRead, 60);

    // A canvas removed for good still frees its context once it is collected:
    // churning contexts through removed canvases keeps the frame drawing.
    for (let i = 0; i < 8; i++) {
        const c = makeCanvas(120);
        const gl = c.getContext('webgl2');
        fill(gl, RED);
        c.remove();
    }
    for (let i = 0; i < 4; i++) { flush(); advanceTime(500); }  // a GC sweep runs about once a second
    flush();
    check('after churn: the first canvas', 0, GREEN, stepRead);

    console.log('test_webgl_reattach PASSED');
}
