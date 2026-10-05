// WebGL work outside requestAnimationFrame, and across frames: drawing from
// top-level script or a timer shows in the composited frame; the app's
// framebuffer binding survives frame boundaries (the engine never rebinds
// the canvas behind its back); two canvases keep their own contents; and a
// long burst of uploads inside one turn neither runs out of upload memory
// nor loses data (the command stream flushes on a budget, not per frame).

document.body.style.margin = '0';
function makeCanvas(left) {
    const c = document.createElement('canvas');
    c.setAttribute('width', '16');
    c.setAttribute('height', '16');
    c.style.position = 'absolute';
    c.style.left = left + 'px';
    c.style.top = '0px';
    document.body.appendChild(c);
    return c;
}
const canvasA = makeCanvas(0);
const canvasB = makeCanvas(32);
flush();

const gl = canvasA.getContext('webgl2');
const gl2 = canvasB.getContext('webgl2');
if (!gl || !gl2) {
    missingGpuContext('webgl2');
} else {
    function near(p, want, msg) {
        const got = [p.r, p.g, p.b];
        assert(got.every((v, i) => Math.abs(v - want[i]) <= 3),
               msg + ': got [' + got + '] want [' + want + ']');
    }

    // --- drawing outside rAF shows up -------------------------------------
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl2.clearColor(0, 0, 1, 1);
    gl2.clear(gl2.COLOR_BUFFER_BIT);
    near(getPixel(8, 8), [0, 255, 0], 'top-level clear of canvas A is composited');
    near(getPixel(40, 8), [0, 0, 255], 'canvas B keeps its own contents');

    let timerRan = false;
    setTimeout(() => {
        gl.clearColor(1, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
        timerRan = true;
    }, 30);
    advanceTime(100);
    assert(timerRan, 'timer ran');
    near(getPixel(8, 8), [255, 0, 0], 'a clear from a timer, frames later, is composited');
    near(getPixel(40, 8), [0, 0, 255], 'canvas B untouched by A');

    // --- the framebuffer binding persists across frames ----------------------
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 16, 16);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    advanceTime(100);   // several frames composite canvas A
    assert(gl.getParameter(gl.FRAMEBUFFER_BINDING) !== null, 'FRAMEBUFFER_BINDING still the FBO after frames');
    gl.clearColor(1, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);   // into the FBO, not the canvas
    advanceTime(50);
    near(getPixel(8, 8), [255, 0, 0], 'a clear after frames went to the bound FBO, not the canvas');
    const px = new Uint8Array(4);
    gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    assert(px[0] === 255 && px[1] === 255 && px[2] === 0, 'the FBO holds the clear: ' + Array.from(px));
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    // --- a burst of uploads in one turn ---------------------------------------
    // 512 x 2 MB of bufferSubData (1 GB) without a frame boundary; each one is
    // drawn from, so none may be dropped and none may be stale.
    const prog = gl.createProgram();
    for (const [type, src] of [
        [gl.VERTEX_SHADER, '#version 300 es\nin vec2 aPos;\nin vec4 aColor;\nout vec4 v;\n' +
                           'void main(){ v = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }'],
        [gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float;\nin vec4 v;\nout vec4 o;\nvoid main(){ o = v; }']]) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        gl.attachShader(prog, s);
    }
    gl.bindAttribLocation(prog, 0, 'aPos');
    gl.bindAttribLocation(prog, 1, 'aColor');
    gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'link: ' + gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    const pos = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, pos);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const big = gl.createBuffer();
    const bytes = 2 * 1024 * 1024;
    gl.bindBuffer(gl.ARRAY_BUFFER, big);
    gl.bufferData(gl.ARRAY_BUFFER, bytes, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(1, 1);    // one color per instance
    const data = new Float32Array(bytes / 4);
    const target = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, target);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 1, 512);
    const rowFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, rowFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
    for (let i = 0; i < 512; ++i) {
        data[0] = (i % 256) / 255;
        data[1] = 1 - (i % 256) / 255;
        data[2] = i < 256 ? 0 : 1;
        data[3] = 1;
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, data);
        gl.viewport(0, i, 1, 1);       // row i records upload i
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, 1);
    }
    assert(gl.getError() === gl.NO_ERROR, 'a 1 GB upload burst raises no error (no OUT_OF_MEMORY)');
    const rows = new Uint8Array(512 * 4);
    gl.readPixels(0, 0, 1, 512, gl.RGBA, gl.UNSIGNED_BYTE, rows);
    let wrong = -1;
    for (let i = 0; i < 512 && wrong < 0; ++i) {
        const r = Math.round(((i % 256) / 255) * 255), b = i < 256 ? 0 : 255;
        if (Math.abs(rows[i * 4] - r) > 1 || Math.abs(rows[i * 4 + 1] - (255 - r)) > 1 || rows[i * 4 + 2] !== b)
            wrong = i;
    }
    assert(wrong < 0, 'every draw saw its own upload (first wrong row ' + wrong + ')');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 16, 16);
    assert(gl.getError() === gl.NO_ERROR, 'end of test');
}
