// BRO_buffer_map — glMapBufferRange exposed as a zero-copy ArrayBuffer.
// WebGL has no equivalent (a browser must not hand a page a raw pointer into
// driver memory), so this is a bro extension, not a conformance surface.
// Exercises src/js/webgl2_bindings_buffers.cpp + src/webgl/webgl2_context.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    function eqArr(a, b, msg) {
        assert(a.length === b.length, msg + ' length');
        for (let i = 0; i < a.length; i++)
            assert(a[i] === b[i], msg + ' [' + i + '] got ' + a[i] + ' want ' + b[i]);
    }
    function drainError() { while (gl.getError() !== gl.NO_ERROR) {} }

    // =====================================================================
    // Feature detection
    // =====================================================================
    const exts = gl.getSupportedExtensions();
    assert(exts.indexOf('BRO_buffer_map') >= 0, 'BRO_buffer_map advertised');

    const M = gl.getExtension('BRO_buffer_map');
    assert(M !== null, 'getExtension returns an object');
    assert(M.MAP_READ_BIT === 0x0001, 'MAP_READ_BIT');
    assert(M.MAP_WRITE_BIT === 0x0002, 'MAP_WRITE_BIT');
    assert(M.MAP_INVALIDATE_RANGE_BIT === 0x0004, 'MAP_INVALIDATE_RANGE_BIT');
    assert(M.MAP_INVALIDATE_BUFFER_BIT === 0x0008, 'MAP_INVALIDATE_BUFFER_BIT');
    assert(M.MAP_FLUSH_EXPLICIT_BIT === 0x0010, 'MAP_FLUSH_EXPLICIT_BIT');
    assert(M.MAP_UNSYNCHRONIZED_BIT === 0x0020, 'MAP_UNSYNCHRONIZED_BIT');
    assert(typeof gl.mapBufferRange === 'function', 'mapBufferRange present');

    // =====================================================================
    // Write through a mapping, read back the conventional way
    // =====================================================================
    const N = 64;
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, N * 4, gl.DYNAMIC_DRAW);
    drainError();

    let ab = gl.mapBufferRange(gl.ARRAY_BUFFER, 0, N * 4,
                               M.MAP_WRITE_BIT | M.MAP_INVALIDATE_BUFFER_BIT);
    assert(ab !== null, 'mapBufferRange returned a buffer');
    assert(ab instanceof ArrayBuffer, 'mapBufferRange returns an ArrayBuffer');
    assert(ab.byteLength === N * 4, 'mapped byteLength matches the request');
    assert(gl.getError() === gl.NO_ERROR, 'map raised no error');

    const want = new Float32Array(N);
    const view = new Float32Array(ab);
    for (let i = 0; i < N; i++) { want[i] = i * 3.25 - 7; view[i] = want[i]; }
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmapBuffer succeeded');

    // The ArrayBuffer aliased driver memory the driver has now taken back;
    // it must be detached, not left pointing at reclaimed pages.
    assert(ab.byteLength === 0, 'ArrayBuffer detached on unmap');
    let threw = false;
    try { view[0] = 1; } catch (e) { threw = true; }
    assert(threw || view.length === 0, 'view over a detached buffer is dead');

    const back = new Float32Array(N);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back);
    eqArr(back, want, 'data written through the mapping');

    // =====================================================================
    // Read mapping sees what bufferSubData wrote
    // =====================================================================
    const patch = new Float32Array([100, 200, 300, 400]);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, patch);
    drainError();
    ab = gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 4 * 4, M.MAP_READ_BIT);
    assert(ab !== null, 'read mapping');
    eqArr(new Float32Array(ab), patch, 'read mapping sees prior writes');
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap read mapping');

    // =====================================================================
    // Sub-range mapping touches only its own range
    // =====================================================================
    const OFF = 16 * 4, LEN = 8 * 4;
    drainError();
    ab = gl.mapBufferRange(gl.ARRAY_BUFFER, OFF, LEN, M.MAP_WRITE_BIT);
    assert(ab !== null && ab.byteLength === LEN, 'sub-range mapping');
    new Float32Array(ab).fill(-1);
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap sub-range');

    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back);
    for (let i = 0; i < N; i++) {
        const inRange = i >= 16 && i < 24;
        // indices 0..3 were overwritten by the bufferSubData patch above
        const expect = inRange ? -1 : (i < 4 ? patch[i] : want[i]);
        assert(back[i] === expect,
               'sub-range isolation at ' + i + ' got ' + back[i] + ' want ' + expect);
    }

    // =====================================================================
    // FLUSH_EXPLICIT: writes land only for the flushed sub-range's mapping
    // =====================================================================
    drainError();
    ab = gl.mapBufferRange(gl.ARRAY_BUFFER, 0, N * 4,
                           M.MAP_WRITE_BIT | M.MAP_FLUSH_EXPLICIT_BIT);
    assert(ab !== null, 'explicit-flush mapping');
    new Float32Array(ab).fill(42);
    gl.flushMappedBufferRange(gl.ARRAY_BUFFER, 0, N * 4);
    assert(gl.getError() === gl.NO_ERROR, 'flushMappedBufferRange raised no error');
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap explicit-flush mapping');
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back);
    for (let i = 0; i < N; i++) assert(back[i] === 42, 'flushed write at ' + i);

    // =====================================================================
    // Error cases
    // =====================================================================
    // Already mapped
    drainError();
    ab = gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16, M.MAP_WRITE_BIT);
    assert(ab !== null, 'first map ok');
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16, M.MAP_WRITE_BIT) === null,
           'second map of the same buffer returns null');
    assert(gl.getError() === gl.INVALID_OPERATION, 'double map is INVALID_OPERATION');
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap after double-map attempt');

    // Unmap with nothing mapped
    drainError();
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === false, 'unmap without a mapping is false');
    assert(gl.getError() === gl.INVALID_OPERATION, 'stray unmap is INVALID_OPERATION');

    // flush with nothing mapped
    drainError();
    gl.flushMappedBufferRange(gl.ARRAY_BUFFER, 0, 16);
    assert(gl.getError() === gl.INVALID_OPERATION, 'stray flush is INVALID_OPERATION');

    // Neither READ nor WRITE
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16, 0) === null, 'access 0 rejected');
    assert(gl.getError() === gl.INVALID_VALUE, 'access 0 is INVALID_VALUE');

    // Unknown access bit
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16, M.MAP_WRITE_BIT | 0x8000) === null,
           'unknown access bit rejected');
    assert(gl.getError() === gl.INVALID_VALUE, 'unknown bit is INVALID_VALUE');

    // INVALIDATE/FLUSH without WRITE
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16,
                             M.MAP_READ_BIT | M.MAP_INVALIDATE_RANGE_BIT) === null,
           'INVALIDATE_RANGE without WRITE rejected');
    assert(gl.getError() === gl.INVALID_OPERATION, 'invalidate w/o write is INVALID_OPERATION');
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16,
                             M.MAP_READ_BIT | M.MAP_FLUSH_EXPLICIT_BIT) === null,
           'FLUSH_EXPLICIT without WRITE rejected');
    assert(gl.getError() === gl.INVALID_OPERATION, 'flush bit w/o write is INVALID_OPERATION');

    // Zero / negative length, negative offset
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 0, M.MAP_WRITE_BIT) === null, 'length 0 rejected');
    assert(gl.getError() === gl.INVALID_VALUE, 'length 0 is INVALID_VALUE');
    drainError();
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, -4, 16, M.MAP_WRITE_BIT) === null,
           'negative offset rejected');
    assert(gl.getError() === gl.INVALID_VALUE, 'negative offset is INVALID_VALUE');

    // Nothing bound to the target
    drainError();
    gl.bindBuffer(gl.ARRAY_BUFFER, null);
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 16, M.MAP_WRITE_BIT) === null,
           'map with no buffer bound rejected');
    assert(gl.getError() === gl.INVALID_OPERATION, 'unbound map is INVALID_OPERATION');
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);

    // =====================================================================
    // deleteBuffer while mapped must not poison a recycled id
    // =====================================================================
    drainError();
    const doomed = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, doomed);
    gl.bufferData(gl.ARRAY_BUFFER, 64, gl.DYNAMIC_DRAW);
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 64, M.MAP_WRITE_BIT) !== null, 'map doomed buffer');
    gl.deleteBuffer(doomed);           // GL unmaps implicitly
    drainError();
    const reborn = gl.createBuffer();   // may reuse the id the driver just freed
    gl.bindBuffer(gl.ARRAY_BUFFER, reborn);
    gl.bufferData(gl.ARRAY_BUFFER, 64, gl.DYNAMIC_DRAW);
    assert(gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 64, M.MAP_WRITE_BIT) !== null,
           'a recycled id is not stuck in the mapped set');
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap reborn');
    gl.deleteBuffer(reborn);

    // =====================================================================
    // A buffer filled through a mapping actually draws
    // =====================================================================
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, '#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p,0,1); }\n');
    gl.compileShader(vs);
    assert(gl.getShaderParameter(vs, gl.COMPILE_STATUS), 'vs compiled');
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){ o = vec4(0,1,0,1); }\n');
    gl.compileShader(fs);
    assert(gl.getShaderParameter(fs, gl.COMPILE_STATUS), 'fs compiled');
    const prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'program linked');

    const tri = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, tri);
    gl.bufferData(gl.ARRAY_BUFFER, 6 * 4, gl.DYNAMIC_DRAW);
    drainError();
    const tab = gl.mapBufferRange(gl.ARRAY_BUFFER, 0, 6 * 4,
                                  M.MAP_WRITE_BIT | M.MAP_INVALIDATE_BUFFER_BIT);
    assert(tab !== null, 'map triangle buffer');
    new Float32Array(tab).set([-1, -1, 3, -1, -1, 3]);   // oversized CCW triangle
    assert(gl.unmapBuffer(gl.ARRAY_BUFFER) === true, 'unmap triangle buffer');

    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, tri);
    const loc = gl.getAttribLocation(prog, 'p');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

    gl.viewport(0, 0, 64, 64);
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(prog);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    const px = new Uint8Array(4);
    gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    assert(px[0] === 0 && px[1] === 255 && px[2] === 0,
           'mapped-write geometry rasterized: got ' + px[0] + ',' + px[1] + ',' + px[2]);
    assert(gl.getError() === gl.NO_ERROR, 'no trailing GL error');

    console.log('webgl map-buffer tests passed');
}
