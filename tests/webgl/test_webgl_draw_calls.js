// WebGL2 draw calls: primitive restart (always on for drawElements), LINE_LOOP,
// 8-bit indices, vertex formats Vulkan has no direct form of (32-bit integers
// read as float, non-normalized bytes), instance divisors above 1, and the
// draw-time validation (index type and alignment, ranges, missing or
// mismatched arrays). Each drawing case is checked by its pixels.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(vs, fs) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            gl.attachShader(p, s);
        }
        gl.bindAttribLocation(p, 0, 'p');
        gl.bindAttribLocation(p, 1, 'c');
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        return p;
    }
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Array.from(b);
    }
    function expectPx(x, y, want, msg) {
        const got = px(x, y);
        assert(got.every((v, i) => Math.abs(v - want[i]) <= 2), msg + ': got [' + got + '] want [' + want + ']');
    }
    function expectError(want, msg) {
        const e = gl.getError();
        assert(e === want, msg + ': error 0x' + e.toString(16) + ' want 0x' + want.toString(16));
    }
    function clear() {
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }
    const BLACK = [0, 0, 0, 255], WHITE = [255, 255, 255, 255];

    // Positions in pixels of the 64x64 canvas, white.
    const solid = program(
        '#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p / 32.0 - 1.0, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){ o = vec4(1.0); }');
    gl.useProgram(solid);
    gl.viewport(0, 0, 64, 64);
    const vbo = gl.createBuffer();
    const ibo = gl.createBuffer();
    function positions(list) {
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(list), gl.STATIC_DRAW);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    }
    function indices(array) {
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, array, gl.STATIC_DRAW);
    }

    // Two quads, left and right, with a gap between them at x 28..36.
    positions([0, 0, 28, 0, 0, 64, 28, 64, 36, 0, 64, 0, 36, 64, 64, 64]);

    // ---- primitive restart: a strip split in two ----
    clear();
    indices(new Uint16Array([0, 1, 2, 3, 0xFFFF, 4, 5, 6, 7]));
    gl.drawElements(gl.TRIANGLE_STRIP, 9, gl.UNSIGNED_SHORT, 0);
    expectPx(10, 32, WHITE, 'restart strip: left quad');
    expectPx(54, 32, WHITE, 'restart strip: right quad');
    expectPx(32, 32, BLACK, 'restart strip: no triangle across the restart');

    clear();
    indices(new Uint8Array([0, 1, 2, 3, 0xFF, 4, 5, 6, 7]));
    gl.drawElements(gl.TRIANGLE_STRIP, 9, gl.UNSIGNED_BYTE, 0);
    expectPx(10, 32, WHITE, '8-bit restart strip: left quad');
    expectPx(54, 32, WHITE, '8-bit restart strip: right quad');
    expectPx(32, 32, BLACK, '8-bit restart strip: 0xFF restarts');

    clear();
    indices(new Uint32Array([0, 1, 2, 0xFFFFFFFF, 5, 4, 6]));
    gl.drawElements(gl.TRIANGLES, 7, gl.UNSIGNED_INT, 0);
    expectPx(4, 8, WHITE, 'restart list: the first triangle');
    expectPx(50, 8, WHITE, 'restart list: the triangle after the restart');
    expectPx(32, 32, BLACK, 'restart list: nothing across it');

    // A partial triangle before a restart is dropped, the rest kept.
    clear();
    indices(new Uint16Array([0, 1, 0xFFFF, 4, 5, 6]));
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
    expectPx(4, 8, BLACK, 'restart list: a partial triangle draws nothing');
    expectPx(50, 8, WHITE, 'restart list: the whole one after it draws');

    // ---- LINE_LOOP closes the loop ----
    clear();
    positions([8.5, 8.5, 55.5, 8.5, 55.5, 55.5, 8.5, 55.5]);
    gl.drawArrays(gl.LINE_LOOP, 0, 4);
    expectPx(32, 8, WHITE, 'line loop: first edge');
    expectPx(8, 32, WHITE, 'line loop: the closing edge');
    clear();
    gl.drawArrays(gl.LINE_STRIP, 0, 4);
    expectPx(8, 32, BLACK, 'line strip: no closing edge');
    clear();
    indices(new Uint8Array([0, 1, 2, 3]));
    gl.drawElements(gl.LINE_LOOP, 4, gl.UNSIGNED_BYTE, 0);
    expectPx(8, 32, WHITE, 'indexed line loop: the closing edge');
    clear();
    indices(new Uint16Array([0, 1, 2, 0xFFFF, 3, 0, 1]));
    gl.drawElements(gl.LINE_LOOP, 7, gl.UNSIGNED_SHORT, 0);
    expectPx(32, 32, WHITE, 'restarted line loop: the first loop closes (2 -> 0, the diagonal)');
    expectPx(8, 32, WHITE, 'restarted line loop: the second loop');

    // ---- vertex formats ----
    // 32-bit integers read as float (no Vulkan vertex format: converted).
    clear();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Int32Array([0, 0, 64, 0, 0, 64, 64, 64]), gl.STATIC_DRAW);
    gl.vertexAttribPointer(0, 2, gl.INT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    expectPx(32, 32, WHITE, 'INT positions read as float');
    // Non-normalized signed bytes (scaled), with a stride and an offset.
    clear();
    gl.bufferData(gl.ARRAY_BUFFER, new Int8Array([99, 99, 0, 0, 99, 99, 64, 0, 99, 99, 0, 64, 99, 99, 64, 64]),
                  gl.STATIC_DRAW);
    gl.vertexAttribPointer(0, 2, gl.BYTE, false, 4, 2);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    expectPx(32, 32, WHITE, 'BYTE positions read as their values');
    // Normalized INT: 2^31 - 1 is 1.0.
    const colored = program(
        '#version 300 es\nin vec2 p;\nin vec4 c;\nout vec4 v;\n' +
        'void main(){ v = c; gl_Position = vec4(p / 32.0 - 1.0, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec4 v;\nout vec4 o;\nvoid main(){ o = v; }');
    gl.useProgram(colored);
    positions([0, 0, 64, 0, 0, 64, 64, 64]);
    const cbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, cbo);
    const half = 0x3FFFFFFF;
    gl.bufferData(gl.ARRAY_BUFFER, new Int32Array([0x7FFFFFFF, half, 0, 0x7FFFFFFF]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.INT, true, 0, 0);
    gl.vertexAttribDivisor(1, 1);
    clear();
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    expectPx(32, 32, [255, 128, 0, 255], 'normalized INT colors');

    // ---- instance divisor 2: instances 0,1 read color 0; 2,3 color 1 ----
    const inst = program(
        '#version 300 es\nin vec2 p;\nin vec4 c;\nout vec4 v;\n' +
        'void main(){ v = c; gl_Position = vec4((p + vec2(float(gl_InstanceID) * 16.0, 0.0)) / 32.0 - 1.0, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec4 v;\nout vec4 o;\nvoid main(){ o = v; }');
    gl.useProgram(inst);
    positions([0, 0, 16, 0, 0, 64, 16, 64]);
    gl.bindBuffer(gl.ARRAY_BUFFER, cbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array([255, 0, 0, 255, 0, 0, 255, 255]), gl.STATIC_DRAW);
    gl.vertexAttribPointer(1, 4, gl.UNSIGNED_BYTE, true, 0, 0);
    gl.vertexAttribDivisor(1, 2);
    clear();
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, 4);
    expectPx(8, 32, [255, 0, 0, 255], 'divisor 2: instance 0');
    expectPx(24, 32, [255, 0, 0, 255], 'divisor 2: instance 1');
    expectPx(40, 32, [0, 0, 255, 255], 'divisor 2: instance 2');
    expectPx(56, 32, [0, 0, 255, 255], 'divisor 2: instance 3');
    expectError(gl.NO_ERROR, 'no error drawing');
    // Five instances read a third color the buffer does not have.
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, 5);
    expectError(gl.INVALID_OPERATION, 'an instanced array read past its end');
    gl.vertexAttribDivisor(1, 0);
    gl.disableVertexAttribArray(1);

    // ---- validation ----
    gl.useProgram(solid);
    positions([0, 0, 64, 0, 0, 64]);
    gl.drawArrays(gl.TRIANGLES, 0, 4);
    expectError(gl.INVALID_OPERATION, 'drawArrays past the end of an array');
    gl.drawArrays(gl.TRIANGLES, -1, 3);
    expectError(gl.INVALID_VALUE, 'negative first');
    gl.drawArrays(0x1234, 0, 3);
    expectError(gl.INVALID_ENUM, 'bad mode');
    indices(new Uint16Array([0, 1, 2, 1, 7]));
    gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 2);
    expectError(gl.NO_ERROR, 'an aligned offset into the indices');
    gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 1);
    expectError(gl.INVALID_OPERATION, 'a misaligned index offset');
    gl.drawElements(gl.TRIANGLES, 3, gl.FLOAT, 0);
    expectError(gl.INVALID_ENUM, 'a bad index type');
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
    expectError(gl.INVALID_OPERATION, 'indices past the end of the buffer');
    gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 4);
    expectError(gl.INVALID_OPERATION, 'index 7 fetches past the array');
    gl.drawRangeElements(gl.TRIANGLES, 2, 1, 3, gl.UNSIGNED_SHORT, 0);
    expectError(gl.INVALID_VALUE, 'drawRangeElements end < start');
    gl.enableVertexAttribArray(3);
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    gl.vertexAttribPointer(3, 4, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.NO_ERROR, 'an enabled array the program does not read is ignored');
    gl.disableVertexAttribArray(3);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 4);
    expectError(gl.INVALID_OPERATION, 'a non-zero offset with no ARRAY_BUFFER');
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 6, 0);
    expectError(gl.INVALID_OPERATION, 'a stride that is not a multiple of the type size');
    gl.vertexAttribPointer(0, 5, gl.FLOAT, false, 0, 0);
    expectError(gl.INVALID_VALUE, 'size 5');
    gl.vertexAttribIPointer(0, 2, gl.FLOAT, 0, 0);
    expectError(gl.INVALID_ENUM, 'vertexAttribIPointer of FLOAT');
    // An integer array into a float input.
    gl.vertexAttribIPointer(0, 2, gl.INT, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    expectError(gl.INVALID_OPERATION, 'an int array into a vec2 input');
}
