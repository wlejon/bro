// WebGL2 conformance subset — vertex array objects: per-VAO attribute and
// element-array state, vertexAttribIPointer integer attributes,
// vertexAttribDivisor + instanced drawing, verified via rendered pixels.
// Exercises src/js/webgl2_bindings_buffers.cpp + draw paths.

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
    function assertPx(p, r, g, b, msg) {
        assert(near(p[0], r) && near(p[1], g) && near(p[2], b),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 frag;\nvoid main(){ frag = uColor; }');
    gl.useProgram(prog);
    const aPos = gl.getAttribLocation(prog, 'aPos');
    const uColor = gl.getUniformLocation(prog, 'uColor');

    // Full-screen strip and left-half strip
    const fullBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, fullBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const halfBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, halfBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 0, -1, -1, 1, 0, 1]), gl.STATIC_DRAW);

    // =====================================================================
    // Attribute state is captured per-VAO
    // =====================================================================
    const vaoFull = gl.createVertexArray();
    gl.bindVertexArray(vaoFull);
    gl.bindBuffer(gl.ARRAY_BUFFER, fullBuf);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);

    const vaoHalf = gl.createVertexArray();
    gl.bindVertexArray(vaoHalf);
    gl.bindBuffer(gl.ARRAY_BUFFER, halfBuf);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);

    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.uniform4f(uColor, 1, 0, 0, 1);
    gl.bindVertexArray(vaoFull);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(48, 32), 255, 0, 0, 'vaoFull covers right half');
    assertPx(px(16, 32), 255, 0, 0, 'vaoFull covers left half');

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindVertexArray(vaoHalf);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(16, 32), 255, 0, 0, 'vaoHalf covers left half');
    assertPx(px(48, 32), 0, 0, 0, 'vaoHalf leaves right half black');

    // Rebinding vaoFull restores its captured pointer (independence)
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindVertexArray(vaoFull);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(48, 32), 255, 0, 0, 'vaoFull state survived vaoHalf setup');

    // =====================================================================
    // ELEMENT_ARRAY_BUFFER binding is per-VAO
    // =====================================================================
    const iboQuad = gl.createBuffer();
    gl.bindVertexArray(vaoFull);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iboQuad);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2, 2, 1, 3]), gl.STATIC_DRAW);

    const iboDegen = gl.createBuffer();
    gl.bindVertexArray(vaoHalf);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iboDegen);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 0, 0, 0, 0, 0]), gl.STATIC_DRAW);

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindVertexArray(vaoFull);
    gl.uniform4f(uColor, 0, 1, 0, 1);
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
    assertPx(px(32, 32), 0, 255, 0, 'vaoFull element buffer draws quad');

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindVertexArray(vaoHalf);
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
    assertPx(px(32, 32), 0, 0, 0, 'vaoHalf degenerate element buffer draws nothing');
    assert(gl.getError() === gl.NO_ERROR, 'no error after per-VAO element buffers');

    // =====================================================================
    // vertexAttribIPointer — integer attributes reach the shader as ints
    // =====================================================================
    const progI = makeProgram(
        '#version 300 es\n' +
        'in vec2 aPos;\nin ivec2 aI;\nout vec4 vCol;\n' +
        'void main(){ gl_Position = vec4(aPos, 0.0, 1.0);\n' +
        '  vCol = vec4(float(aI.x)/255.0, float(aI.y)/255.0, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec4 vCol;\nout vec4 frag;\nvoid main(){ frag = vCol; }');
    gl.useProgram(progI);
    const aPosI = gl.getAttribLocation(progI, 'aPos');
    const aI = gl.getAttribLocation(progI, 'aI');
    assert(aPosI >= 0 && aI >= 0, 'integer attrib locations');

    const vaoI = gl.createVertexArray();
    gl.bindVertexArray(vaoI);
    gl.bindBuffer(gl.ARRAY_BUFFER, fullBuf);
    gl.enableVertexAttribArray(aPosI);
    gl.vertexAttribPointer(aPosI, 2, gl.FLOAT, false, 0, 0);
    const intBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, intBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Int32Array([128, 64, 128, 64, 128, 64, 128, 64]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aI);
    gl.vertexAttribIPointer(aI, 2, gl.INT, 0, 0);

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(32, 32), 128, 64, 0, 'integer attribute values');
    assert(gl.getError() === gl.NO_ERROR, 'no error after vertexAttribIPointer');

    // =====================================================================
    // vertexAttribDivisor + drawArraysInstanced — N instances, N positions
    // =====================================================================
    const progInst = makeProgram(
        '#version 300 es\n' +
        'in vec2 aPos;\nin vec2 aOff;\n' +
        'void main(){ gl_Position = vec4(aPos * 0.12 + aOff, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nout vec4 frag;\nvoid main(){ frag = vec4(1.0, 0.0, 1.0, 1.0); }');
    gl.useProgram(progInst);
    const aPos2 = gl.getAttribLocation(progInst, 'aPos');
    const aOff = gl.getAttribLocation(progInst, 'aOff');

    const vaoInst = gl.createVertexArray();
    gl.bindVertexArray(vaoInst);
    gl.bindBuffer(gl.ARRAY_BUFFER, fullBuf);
    gl.enableVertexAttribArray(aPos2);
    gl.vertexAttribPointer(aPos2, 2, gl.FLOAT, false, 0, 0);
    const offBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, offBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-0.6, 0, 0, 0, 0.6, 0]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(aOff);
    gl.vertexAttribPointer(aOff, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(aOff, 1);

    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, 3);
    // Instance centers: NDC -0.6, 0, 0.6 -> px 13, 32, 51; gaps at 22, 42
    assertPx(px(13, 32), 255, 0, 255, 'instance 0 rendered');
    assertPx(px(32, 32), 255, 0, 255, 'instance 1 rendered');
    assertPx(px(51, 32), 255, 0, 255, 'instance 2 rendered');
    assertPx(px(22, 32), 0, 0, 0, 'gap between instances 0-1 empty');
    assertPx(px(42, 32), 0, 0, 0, 'gap between instances 1-2 empty');

    // drawElementsInstanced with the same layout
    const iboInst = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iboInst);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2, 2, 1, 3]), gl.STATIC_DRAW);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawElementsInstanced(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0, 2);
    assertPx(px(13, 32), 255, 0, 255, 'instanced elements instance 0');
    assertPx(px(32, 32), 255, 0, 255, 'instanced elements instance 1');
    assertPx(px(51, 32), 0, 0, 0, 'instanced elements draws only 2 instances');
    assert(gl.getError() === gl.NO_ERROR, 'no error after instancing');

    // =====================================================================
    // Cleanup — delete while bound and after unbind
    // =====================================================================
    gl.bindVertexArray(null);
    gl.deleteVertexArray(vaoFull);
    gl.deleteVertexArray(vaoHalf);
    gl.deleteVertexArray(vaoI);
    gl.deleteVertexArray(vaoInst);
    gl.deleteVertexArray(vaoInst); // double delete no-op
    gl.deleteBuffer(fullBuf);
    gl.deleteBuffer(halfBuf);
    gl.deleteBuffer(intBuf);
    gl.deleteBuffer(offBuf);
    gl.deleteBuffer(iboQuad);
    gl.deleteBuffer(iboDegen);
    gl.deleteBuffer(iboInst);
    gl.deleteProgram(prog);
    gl.deleteProgram(progI);
    gl.deleteProgram(progInst);
    assert(gl.getError() === gl.NO_ERROR, 'no error after cleanup');

    console.log('webgl vao tests passed');
}

document.body.removeChild(canvas);
