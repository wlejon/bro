// WebGL2 conformance subset — pixel buffer objects and framebuffer copies:
// readPixels into a PIXEL_PACK PBO (verified byte-for-byte against direct
// readPixels), texture upload from a PIXEL_UNPACK PBO (verified via pixels),
// the WebGL2 client-memory-vs-PBO INVALID_OPERATION rules, PBO bounds
// checking, and copyTexImage2D/copyTexSubImage2D round-trips.
// Exercises src/js/webgl2_bindings_framebuffers.cpp, _textures.cpp +
// src/webgl/webgl2_context.cpp.

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

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    // Reference frame: red left half, blue right half.
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(32, 0, 32, 64);
    gl.clearColor(0, 0, 1, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.disable(gl.SCISSOR_TEST);

    // =====================================================================
    // readPixels -> PACK PBO -> getBufferSubData, vs direct readPixels
    // =====================================================================
    const direct = new Uint8Array(64 * 64 * 4);
    gl.readPixels(0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, direct);

    const pack = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pack);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, 64 * 64 * 4, gl.STREAM_READ);
    gl.readPixels(0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    assert(gl.getError() === gl.NO_ERROR, 'readPixels into PBO');
    const viaPbo = new Uint8Array(64 * 64 * 4);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, viaPbo);
    let same = true;
    for (let i = 0; i < direct.length; i++) {
        if (direct[i] !== viaPbo[i]) { same = false; break; }
    }
    assert(same, 'PBO readback matches direct readPixels byte-for-byte');

    // Client-memory readPixels while a PACK PBO is bound is INVALID_OPERATION
    // and must not touch the destination array.
    const untouched = new Uint8Array(4).fill(7);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, untouched);
    assert(gl.getError() === gl.INVALID_OPERATION,
           'client readPixels with PACK PBO bound -> INVALID_OPERATION');
    assert(untouched[0] === 7 && untouched[3] === 7, 'destination array untouched');

    // Offset past the PBO's capacity: INVALID_OPERATION, no write.
    gl.readPixels(0, 0, 64, 64, gl.RGBA, gl.UNSIGNED_BYTE, 4);
    assert(gl.getError() === gl.INVALID_OPERATION,
           'PBO offset overflow -> INVALID_OPERATION');
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);

    // Offset overload without a PACK PBO bound: INVALID_OPERATION.
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    assert(gl.getError() === gl.INVALID_OPERATION,
           'offset readPixels without PBO -> INVALID_OPERATION');

    // =====================================================================
    // texImage2D from an UNPACK PBO (offset overload), proven via pixels
    // =====================================================================
    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
        'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec2 vUv;\nuniform sampler2D uTex;\nout vec4 frag;\n' +
        'void main(){ frag = texture(uTex, vUv); }');
    gl.useProgram(prog);
    gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);
    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    // PBO payload: 4 bytes padding then a solid-green 2x2 RGBA image.
    const texels = new Uint8Array(4 + 16);
    for (let i = 0; i < 4; i++) texels.set([0, 255, 0, 255], 4 + i * 4);
    const unpack = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, unpack);
    gl.bufferData(gl.PIXEL_UNPACK_BUFFER, texels, gl.STREAM_DRAW);

    const tex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, 4);
    assert(gl.getError() === gl.NO_ERROR, 'texImage2D from PBO at offset');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(32, 32), 0, 255, 0, 'PBO-sourced texture drew green');

    // Client-memory upload while an UNPACK PBO is bound: INVALID_OPERATION.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([255, 0, 0, 255]));
    assert(gl.getError() === gl.INVALID_OPERATION,
           'client texImage2D with UNPACK PBO bound -> INVALID_OPERATION');
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE,
                     new Uint8Array([255, 0, 0, 255]));
    assert(gl.getError() === gl.INVALID_OPERATION,
           'client texSubImage2D with UNPACK PBO bound -> INVALID_OPERATION');
    // null upload (allocate-only) stays legal with a PBO bound.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    assert(gl.getError() === gl.NO_ERROR, 'null allocation with UNPACK PBO bound is legal');

    // FLIP_Y with a PBO source: INVALID_OPERATION (transforms are
    // client-memory-only in WebGL2).
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, 4);
    assert(gl.getError() === gl.INVALID_OPERATION, 'FLIP_Y + PBO source -> INVALID_OPERATION');
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);

    // texSubImage2D from the PBO: replace with green again after re-alloc.
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, 4);
    assert(gl.getError() === gl.NO_ERROR, 'texSubImage2D from PBO at offset');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(32, 32), 0, 255, 0, 'PBO-sourced texSubImage2D drew green');
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);

    // =====================================================================
    // copyTexImage2D / copyTexSubImage2D
    // =====================================================================
    // Canvas: solid red. Copy the framebuffer into a texture, draw it.
    gl.clearColor(1, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const copyTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, copyTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.copyTexImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 0, 0, 64, 64, 0);
    assert(gl.getError() === gl.NO_ERROR, 'copyTexImage2D from canvas');

    // Now paint the canvas green and copy its lower-left 32x32 into the
    // texture's lower-left quadrant.
    gl.clearColor(0, 1, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, 32, 32);
    assert(gl.getError() === gl.NO_ERROR, 'copyTexSubImage2D from canvas');

    // Draw the texture fullscreen: lower-left quadrant green, rest red.
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assertPx(px(10, 10), 0, 255, 0, 'copyTexSubImage2D region is green');
    assertPx(px(50, 50), 255, 0, 0, 'copyTexImage2D region is red');
    assert(gl.getError() === gl.NO_ERROR, 'no error after copy round-trip');

    gl.deleteBuffer(pack);
    gl.deleteBuffer(unpack);
    gl.deleteTexture(tex);
    gl.deleteTexture(copyTex);

    console.log('webgl pbo tests passed');
}

document.body.removeChild(canvas);
