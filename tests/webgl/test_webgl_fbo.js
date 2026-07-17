// WebGL2 conformance subset — framebuffers & renderbuffers: attachment
// combos, checkFramebufferStatus, multisample renderbuffer + blitFramebuffer
// resolve, readPixels format/type combos + destination-size validation,
// drawBuffers MRT with readBuffer, getFragDataLocation.
// Exercises src/js/webgl2_bindings_framebuffers.cpp + context FBO paths.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    function makeProgram(vsSrc, fsSrc) {
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSrc); gl.compileShader(vs);
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS))
            throw new Error('vs: ' + gl.getShaderInfoLog(vs));
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSrc); gl.compileShader(fs);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS))
            throw new Error('fs: ' + gl.getShaderInfoLog(fs));
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS))
            throw new Error('link: ' + gl.getProgramInfoLog(p));
        gl.deleteShader(vs); gl.deleteShader(fs);
        return p;
    }
    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 3); }
    function assertPx(p, r, g, b, msg, tol) {
        assert(near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    // =====================================================================
    // Completeness: fresh FBO with no attachments is incomplete
    // =====================================================================
    const fboEmpty = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboEmpty);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
           'no attachments -> INCOMPLETE_MISSING_ATTACHMENT');

    // =====================================================================
    // Renderbuffer color + depth attachment combos
    // =====================================================================
    const rboColor = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rboColor);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, 16, 16);
    const rboDepth = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rboDepth);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, 16, 16);

    const fboRB = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboRB);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rboColor);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, rboDepth);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
           'RGBA8 rbo + DEPTH_COMPONENT16 rbo complete');
    gl.viewport(0, 0, 16, 16);
    gl.clearColor(0.2, 0.4, 0.6, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    const rbPx = new Uint8Array(4);
    gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, rbPx);
    assertPx(rbPx, 51, 102, 153, 'clear renders into renderbuffer-backed FBO');

    // DEPTH24_STENCIL8 renderbuffer at DEPTH_STENCIL_ATTACHMENT
    const rboDS = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rboDS);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, 16, 16);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, null);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, rboDS);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
           'DEPTH24_STENCIL8 at DEPTH_STENCIL_ATTACHMENT complete');
    assert(gl.getError() === gl.NO_ERROR, 'no error in attachment combos');

    // =====================================================================
    // Multisample renderbuffer + blitFramebuffer resolve
    // =====================================================================
    const msColor = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, msColor);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, 4, gl.RGBA8, 32, 32);
    const fboMS = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboMS);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, msColor);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
           '4x multisample RGBA8 complete');

    const progRed = makeProgram(
        '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nout vec4 frag;\nvoid main(){ frag = vec4(1.0, 0.0, 0.0, 1.0); }');
    gl.viewport(0, 0, 32, 32);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(progRed);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    const resolveTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, resolveTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 32, 32, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    const fboResolve = gl.createFramebuffer();
    gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fboMS);
    gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, fboResolve);
    gl.framebufferTexture2D(gl.DRAW_FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, resolveTex, 0);
    gl.blitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, gl.COLOR_BUFFER_BIT, gl.NEAREST);
    assert(gl.getError() === gl.NO_ERROR, 'multisample resolve blit no error');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboResolve);
    const msPx = new Uint8Array(4);
    gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, msPx);
    assertPx(msPx, 255, 0, 0, 'resolved multisample interior');

    // =====================================================================
    // readPixels format/type combos
    // =====================================================================
    // Implementation-defined pair is queryable while an FBO is bound
    const implFmt = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT);
    const implType = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE);
    assert(implFmt > 0 && implType > 0, 'impl read format/type queryable');

    // Float readback from an RGBA32F renderbuffer (EXT_color_buffer_float)
    const rboF = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rboF);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA32F, 4, 4);
    const fboF = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboF);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rboF);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'RGBA32F rbo complete');
    gl.clearColor(0.25, 0.5, 2.0, 1.0); // >1 survives in float target
    gl.clear(gl.COLOR_BUFFER_BIT);
    const fPix = new Float32Array(4);
    gl.readPixels(1, 1, 1, 1, gl.RGBA, gl.FLOAT, fPix);
    assert(Math.abs(fPix[0] - 0.25) < 0.01 && Math.abs(fPix[1] - 0.5) < 0.01 &&
           Math.abs(fPix[2] - 2.0) < 0.01, 'RGBA/FLOAT readback incl. >1 values (got ' +
           Array.from(fPix).join(',') + ')');
    gl.clearColor(0, 0, 0, 0);

    // =====================================================================
    // readPixels destination-size validation (no out-of-bounds write)
    // =====================================================================
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboResolve);
    const tiny = new Uint8Array(16); // 4x4 RGBA needs 64 bytes
    tiny.fill(7);
    gl.readPixels(0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, tiny);
    assert(gl.getError() === gl.INVALID_OPERATION, 'too-small dest -> INVALID_OPERATION');
    let untouched = true;
    for (let i = 0; i < 16; i++) if (tiny[i] !== 7) untouched = false;
    assert(untouched, 'too-small dest untouched');
    assert(gl.getError() === gl.NO_ERROR, 'synthetic error cleared after read');

    // =====================================================================
    // MRT: drawBuffers + readBuffer + getFragDataLocation
    // =====================================================================
    const mrtA = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, mrtA);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 8, 8, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    const mrtB = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, mrtB);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 8, 8, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);

    const fboMRT = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboMRT);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, mrtA, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, mrtB, 0);
    assert(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE, 'MRT fbo complete');

    const progMRT = makeProgram(
        '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\n' +
        'layout(location = 0) out vec4 outA;\nlayout(location = 1) out vec4 outB;\n' +
        'void main(){ outA = vec4(1.0, 0.0, 0.0, 1.0); outB = vec4(0.0, 1.0, 0.0, 1.0); }');
    assert(gl.getFragDataLocation(progMRT, 'outA') === 0, 'getFragDataLocation outA = 0');
    assert(gl.getFragDataLocation(progMRT, 'outB') === 1, 'getFragDataLocation outB = 1');
    assert(gl.getFragDataLocation(progMRT, 'nope') === -1, 'getFragDataLocation missing = -1');

    gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1]);
    gl.viewport(0, 0, 8, 8);
    gl.useProgram(progMRT);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    const mrtPx = new Uint8Array(4);
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, mrtPx);
    assertPx(mrtPx, 255, 0, 0, 'MRT attachment 0 written red');
    gl.readBuffer(gl.COLOR_ATTACHMENT1);
    gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, mrtPx);
    assertPx(mrtPx, 0, 255, 0, 'MRT attachment 1 written green');
    gl.readBuffer(gl.COLOR_ATTACHMENT0);
    assert(gl.getError() === gl.NO_ERROR, 'no error after MRT');

    // drawBuffers with NONE holes
    gl.drawBuffers([gl.NONE, gl.COLOR_ATTACHMENT1]);
    assert(gl.getError() === gl.NO_ERROR, 'drawBuffers with NONE accepted');
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);

    // =====================================================================
    // Reading from the canvas default framebuffer still works after all this
    // =====================================================================
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);
    gl.clearColor(0, 0.5, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const canvasPx = new Uint8Array(4);
    gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, canvasPx);
    assertPx(canvasPx, 0, 128, 0, 'canvas default framebuffer readback');

    // Cleanup — includes deleting an FBO while bound
    gl.bindFramebuffer(gl.FRAMEBUFFER, fboMRT);
    gl.deleteFramebuffer(fboMRT);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fboEmpty);
    gl.deleteFramebuffer(fboRB);
    gl.deleteFramebuffer(fboMS);
    gl.deleteFramebuffer(fboResolve);
    gl.deleteFramebuffer(fboF);
    gl.deleteRenderbuffer(rboColor);
    gl.deleteRenderbuffer(rboDepth);
    gl.deleteRenderbuffer(rboDS);
    gl.deleteRenderbuffer(msColor);
    gl.deleteRenderbuffer(rboF);
    gl.deleteTexture(resolveTex);
    gl.deleteTexture(mrtA);
    gl.deleteTexture(mrtB);
    gl.deleteBuffer(quadBuf);
    gl.deleteVertexArray(vao);
    gl.deleteProgram(progRed);
    gl.deleteProgram(progMRT);
    assert(gl.getError() === gl.NO_ERROR, 'no error after cleanup');

    console.log('webgl fbo tests passed');
}

document.body.removeChild(canvas);
