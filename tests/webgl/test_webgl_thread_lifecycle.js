// The WebGL thread's lifecycle seen from a page: every context's backend
// lives on one engine thread that replays what the page encodes. Many
// contexts share it; a resize, a context loss and a restore land in order
// with commands still pending; a canvas collected with a backlog in its ring
// goes without stalling the others.

function canvasOf(w, h) {
    const c = document.createElement('canvas');
    c.setAttribute('width', String(w));
    c.setAttribute('height', String(h));
    document.body.appendChild(c);
    return c;
}

function pixel(gl, x, y) {
    const b = new Uint8Array(4);
    gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
    return Array.from(b);
}

function same(a, b) { return a.length === b.length && a.every((v, i) => v === b[i]); }

const first = canvasOf(16, 16);
flush();
const probe = first.getContext('webgl2');
if (!probe) {
    missingGpuContext('webgl2');
} else {
    // --- Many contexts, their calls interleaved -------------------------------
    const canvases = [first];
    for (let i = 1; i < 6; i++) canvases.push(canvasOf(16, 16));
    flush();
    const gls = canvases.map((c) => c.getContext('webgl2', { preserveDrawingBuffer: true }));
    for (let round = 0; round < 300; round++) {
        gls.forEach((gl, i) => {
            gl.clearColor(i / 255, (round & 255) / 255, 0, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
        });
    }
    gls.forEach((gl, i) => {
        assert(same(pixel(gl, 3, 3), [i, 299 & 255, 0, 255]), 'context ' + i + ' drew its own color');
        assert(gl.getError() === gl.NO_ERROR, 'context ' + i + ': no error');
    });
    advanceTime(16);
    gls.forEach((gl, i) => assert(same(pixel(gl, 3, 3), [i, 299 & 255, 0, 255]),
                                  'context ' + i + ' kept its color across a frame'));

    // --- A resize between a frame's commands ----------------------------------
    {
        const gl = gls[1];
        gl.clearColor(1, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        for (let k = 0; k < 2000; k++) gl.viewport(0, 0, 8 + (k & 7), 8);
        canvases[1].setAttribute('width', '40');
        advanceTime(16);
        assert(gl.drawingBufferWidth === 40, 'the drawing buffer took the new width');
        gl.viewport(0, 0, 40, 16);
        gl.clearColor(0, 1, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        assert(same(pixel(gl, 39, 15), [0, 255, 0, 255]), 'drawn at the new size');
        assert(gl.getError() === gl.NO_ERROR, 'resize: no error');
    }

    // --- Lose and restore with commands pending -------------------------------
    {
        const gl = gls[2];
        const ext = gl.getExtension('WEBGL_lose_context');
        let lost = 0, restored = 0;
        canvases[2].addEventListener('webglcontextlost', (e) => { ++lost; e.preventDefault(); });
        canvases[2].addEventListener('webglcontextrestored', () => { ++restored; });
        for (let rep = 0; rep < 3; rep++) {
            const buf = gl.createBuffer();
            gl.bindBuffer(gl.ARRAY_BUFFER, buf);
            gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(64 * 1024), gl.DYNAMIC_DRAW);  // a blob
            for (let k = 0; k < 3000; k++) {
                gl.clearColor(1, 0, 1, 1);
                gl.clear(gl.COLOR_BUFFER_BIT);
            }
            ext.loseContext();
            assert(gl.isContextLost(), 'lost at once');
            assert(gl.getError() === gl.CONTEXT_LOST_WEBGL, 'CONTEXT_LOST_WEBGL once');
            advanceTime(16);
            assert(lost === rep + 1, 'webglcontextlost fired');
            ext.restoreContext();
            advanceTime(16);
            assert(restored === rep + 1 && !gl.isContextLost(), 'restored');
            assert(same(pixel(gl, 1, 1), [0, 0, 0, 0]), 'a restored drawing buffer starts cleared');
            gl.clearColor(0, 0, 1, 1);
            gl.clear(gl.COLOR_BUFFER_BIT);
            assert(same(pixel(gl, 1, 1), [0, 0, 255, 255]), 'the restored context draws');
            assert(gl.getError() === gl.NO_ERROR, 'restore: no error');
        }
    }

    // --- Canvases collected with a backlog in their rings ---------------------
    for (let rep = 0; rep < 4; rep++) {
        let c = canvasOf(32, 32);
        flush();
        let gl = c.getContext('webgl2');
        const buf = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, buf);
        for (let k = 0; k < 20; k++) gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array(256 * 1024), gl.STATIC_DRAW);
        for (let k = 0; k < 20000; k++) gl.clear(gl.COLOR_BUFFER_BIT);
        gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
        document.body.removeChild(c);
        c = null;
        gl = null;
        __host.gc();
        advanceTime(16);
    }
    for (let k = 0; k < 4; k++) {
        __host.gc();
        advanceTime(16);
    }
    gls.forEach((gl, i) => {
        gl.clearColor(0, i / 255, 1, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        assert(same(pixel(gl, 0, 0), [0, i, 255, 255]), 'context ' + i + ' still draws after the others went');
    });
    advanceTime(16);
}
