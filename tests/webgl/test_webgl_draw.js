// WebGL2 conformance subset — drawing state verified numerically via pixels:
// scissor, blending, depth test ordering, stencil masking, colorMask,
// drawElements with all three index types, drawRangeElements, point
// rendering with gl_PointSize.
// Exercises src/js/webgl2_bindings_state.cpp + context draw paths.

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
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return b;
    }
    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 3); }
    function assertPx(p, r, g, b, msg, tol) {
        assert(near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }

    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nuniform float uZ;\nuniform float uPointSize;\n' +
        'void main(){ gl_Position = vec4(aPos, uZ, 1.0); gl_PointSize = uPointSize; }',
        '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 frag;\n' +
        'void main(){ frag = uColor; }');
    gl.useProgram(prog);
    const uColor = gl.getUniformLocation(prog, 'uColor');
    const uZ = gl.getUniformLocation(prog, 'uZ');
    const uPointSize = gl.getUniformLocation(prog, 'uPointSize');
    gl.uniform1f(uZ, 0);
    gl.uniform1f(uPointSize, 1);

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    function drawQuad(r, g, b, a, z) {
        gl.uniform4f(uColor, r, g, b, a);
        gl.uniform1f(uZ, z || 0);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }

    // =====================================================================
    // Scissor clips both clear and draw
    // =====================================================================
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 32, 64);
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    assertPx(px(16, 32), 0, 0, 255, 'scissored clear inside');
    assertPx(px(48, 32), 255, 0, 0, 'scissored clear outside untouched');
    drawQuad(0, 1, 0, 1, 0);
    assertPx(px(16, 32), 0, 255, 0, 'scissored draw inside');
    assertPx(px(48, 32), 255, 0, 0, 'scissored draw outside untouched');
    gl.disable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 64, 64);

    // =====================================================================
    // Blending — SRC_ALPHA / ONE_MINUS_SRC_ALPHA verified numerically
    // =====================================================================
    gl.clearColor(0.5, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.blendEquation(gl.FUNC_ADD);
    drawQuad(0, 1, 0, 0.5, 0);
    // result = 0.5*src + 0.5*dst -> r=0.25, g=0.5
    assertPx(px(32, 32), 64, 128, 0, 'alpha blend result', 4);

    // FUNC_REVERSE_SUBTRACT with ONE/ONE: result = dst - src
    gl.clearColor(0.75, 0.5, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.blendFunc(gl.ONE, gl.ONE);
    gl.blendEquation(gl.FUNC_REVERSE_SUBTRACT);
    drawQuad(0.25, 0.25, 0, 1, 0);
    assertPx(px(32, 32), 128, 64, 0, 'reverse-subtract blend', 4);
    gl.blendEquation(gl.FUNC_ADD);
    gl.disable(gl.BLEND);

    // =====================================================================
    // Depth test — near wins regardless of draw order
    // =====================================================================
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LESS);
    gl.clearColor(0, 0, 0, 1);
    gl.clearDepth(1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    drawQuad(0, 1, 0, 1, 0.5);  // far green first
    drawQuad(1, 0, 0, 1, -0.5); // near red second
    assertPx(px(32, 32), 255, 0, 0, 'near-after-far: near wins');
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    drawQuad(1, 0, 0, 1, -0.5); // near red first
    drawQuad(0, 1, 0, 1, 0.5);  // far green second — rejected
    assertPx(px(32, 32), 255, 0, 0, 'far-after-near: near still wins');

    // depthMask(false) freezes the depth buffer
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.depthMask(false);
    drawQuad(0, 1, 0, 1, -0.5); // drawn but writes no depth
    gl.depthMask(true);
    drawQuad(1, 0, 0, 1, 0.0);  // passes because depth stayed 1.0
    assertPx(px(32, 32), 255, 0, 0, 'depthMask(false) leaves depth buffer');
    gl.disable(gl.DEPTH_TEST);

    // =====================================================================
    // Stencil — write a mask region, then draw through it
    // =====================================================================
    gl.enable(gl.STENCIL_TEST);
    gl.clearColor(0, 0, 0, 1);
    gl.clearStencil(0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    // Pass 1: stamp stencil=1 on the left half, no color writes
    gl.stencilFunc(gl.ALWAYS, 1, 0xFF);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.REPLACE);
    gl.stencilMask(0xFF);
    gl.colorMask(false, false, false, false);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, 32, 64);
    drawQuad(1, 1, 1, 1, 0);
    gl.disable(gl.SCISSOR_TEST);
    gl.colorMask(true, true, true, true);
    // Pass 2: draw only where stencil == 1
    gl.stencilFunc(gl.EQUAL, 1, 0xFF);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.KEEP);
    drawQuad(0, 1, 1, 1, 0);
    assertPx(px(16, 32), 0, 255, 255, 'stencil==1 region drawn');
    assertPx(px(48, 32), 0, 0, 0, 'stencil==0 region masked out');
    gl.disable(gl.STENCIL_TEST);

    // =====================================================================
    // colorMask channels
    // =====================================================================
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.colorMask(false, true, false, true);
    drawQuad(1, 1, 1, 1, 0);
    gl.colorMask(true, true, true, true);
    assertPx(px(32, 32), 0, 255, 0, 'colorMask writes only green');

    // =====================================================================
    // drawElements with all three index types
    // =====================================================================
    const indices = [0, 1, 2, 2, 1, 3];
    for (const [type, Arr, name] of [
        [gl.UNSIGNED_BYTE, Uint8Array, 'UNSIGNED_BYTE'],
        [gl.UNSIGNED_SHORT, Uint16Array, 'UNSIGNED_SHORT'],
        [gl.UNSIGNED_INT, Uint32Array, 'UNSIGNED_INT'],
    ]) {
        const ibo = gl.createBuffer();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Arr(indices), gl.STATIC_DRAW);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.uniform4f(uColor, 1, 0, 1, 1);
        gl.uniform1f(uZ, 0);
        gl.drawElements(gl.TRIANGLES, 6, type, 0);
        assertPx(px(32, 32), 255, 0, 255, 'drawElements ' + name);
        gl.deleteBuffer(ibo);
    }

    // drawRangeElements
    const iboR = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iboR);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(indices), gl.STATIC_DRAW);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.uniform4f(uColor, 1, 1, 0, 1);
    gl.drawRangeElements(gl.TRIANGLES, 0, 3, 6, gl.UNSIGNED_SHORT, 0);
    assertPx(px(32, 32), 255, 255, 0, 'drawRangeElements');
    gl.deleteBuffer(iboR);
    assert(gl.getError() === gl.NO_ERROR, 'no error after index-type draws');

    // =====================================================================
    // POINTS with gl_PointSize (PROGRAM_POINT_SIZE enabled by the context)
    // =====================================================================
    const pointBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, pointBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0]), gl.STATIC_DRAW);
    const vaoP = gl.createVertexArray();
    gl.bindVertexArray(vaoP);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.uniform4f(uColor, 0, 1, 0, 1);
    gl.uniform1f(uPointSize, 32);
    gl.drawArrays(gl.POINTS, 0, 1);
    assertPx(px(32, 32), 0, 255, 0, 'point center rendered');
    assertPx(px(42, 32), 0, 255, 0, 'gl_PointSize expands the point');
    assertPx(px(60, 32), 0, 0, 0, 'point does not cover everything');
    assert(gl.getError() === gl.NO_ERROR, 'no error after points');

    // LINES draw path
    const lineBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, lineBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, 0, 1, 0]), gl.STATIC_DRAW);
    const vaoL = gl.createVertexArray();
    gl.bindVertexArray(vaoL);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.lineWidth(1);
    gl.uniform4f(uColor, 1, 0, 0, 1);
    gl.drawArrays(gl.LINES, 0, 2);
    const linePx = px(32, 32);
    const lineAlt = px(32, 31);
    assert(linePx[0] === 255 || lineAlt[0] === 255, 'horizontal line rendered');
    assert(gl.getError() === gl.NO_ERROR, 'no error after lines');

    // polygonOffset accepted
    gl.enable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(1, 1);
    drawQuad(0, 0, 1, 1, 0);
    gl.disable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(0, 0);
    assert(gl.getError() === gl.NO_ERROR, 'no error after polygonOffset');

    // Cleanup
    gl.deleteBuffer(quadBuf);
    gl.deleteBuffer(pointBuf);
    gl.deleteBuffer(lineBuf);
    gl.deleteVertexArray(vao);
    gl.deleteVertexArray(vaoP);
    gl.deleteVertexArray(vaoL);
    gl.deleteProgram(prog);
    assert(gl.getError() === gl.NO_ERROR, 'no error after cleanup');

    console.log('webgl draw tests passed');
}

document.body.removeChild(canvas);
