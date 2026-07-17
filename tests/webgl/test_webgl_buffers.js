// WebGL2 conformance subset — buffer objects: bufferData (all signatures),
// bufferSubData with element-unit srcOffset/length, copyBufferSubData,
// getBufferSubData round-trips, WebGL2 binding points, error cases.
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

    // =====================================================================
    // bufferData(view) + full getBufferSubData round-trip
    // =====================================================================
    const b1 = gl.createBuffer();
    assert(b1 !== null, 'createBuffer');
    gl.bindBuffer(gl.ARRAY_BUFFER, b1);
    const f16 = new Float32Array(16);
    for (let i = 0; i < 16; i++) f16[i] = i * 1.5 - 4;
    gl.bufferData(gl.ARRAY_BUFFER, f16, gl.STATIC_DRAW);
    const back = new Float32Array(16);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back);
    eqArr(back, f16, 'float round-trip');

    // =====================================================================
    // bufferData(ArrayBuffer)
    // =====================================================================
    const ab = new ArrayBuffer(8);
    new Uint8Array(ab).set([1, 2, 3, 4, 5, 6, 7, 8]);
    gl.bufferData(gl.ARRAY_BUFFER, ab, gl.DYNAMIC_DRAW);
    const back8 = new Uint8Array(8);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back8);
    eqArr(back8, new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), 'ArrayBuffer round-trip');

    // =====================================================================
    // bufferData(size) + bufferSubData pattern write
    // =====================================================================
    gl.bufferData(gl.ARRAY_BUFFER, 32, gl.DYNAMIC_DRAW);
    const pat = new Uint8Array(32);
    for (let i = 0; i < 32; i++) pat[i] = (i * 7 + 3) & 0xFF;
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, pat);
    const back32 = new Uint8Array(32);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back32);
    eqArr(back32, pat, 'size-alloc + subdata round-trip');

    // Partial bufferSubData at a byte offset
    gl.bufferSubData(gl.ARRAY_BUFFER, 8, new Uint8Array([0xAA, 0xBB]));
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back32);
    assert(back32[8] === 0xAA && back32[9] === 0xBB && back32[7] === pat[7] && back32[10] === pat[10],
           'partial subdata at offset');

    // =====================================================================
    // WebGL2 srcOffset/length are ELEMENT units of the source typed array
    // =====================================================================
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(8), gl.DYNAMIC_DRAW);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([10, 11, 12, 13, 14]), 1, 2);
    const backF8 = new Float32Array(8);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, backF8);
    eqArr(backF8, new Float32Array([11, 12, 0, 0, 0, 0, 0, 0]),
          'bufferSubData srcOffset/length in elements');

    // bufferData(srcData, usage, srcOffset, length)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([1, 2, 3, 4, 5, 6]), gl.STATIC_DRAW, 2, 3);
    const back3 = new Float32Array(3);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, back3);
    eqArr(back3, new Float32Array([3, 4, 5]), 'bufferData srcOffset/length in elements');

    // getBufferSubData(dstBuffer, dstOffset, length)
    const dst6 = new Float32Array([9, 9, 9, 9, 9, 9]);
    gl.getBufferSubData(gl.ARRAY_BUFFER, 0, dst6, 2, 3);
    eqArr(dst6, new Float32Array([9, 9, 3, 4, 5, 9]), 'getBufferSubData dstOffset/length');

    // =====================================================================
    // copyBufferSubData via COPY_READ/COPY_WRITE
    // =====================================================================
    const src = gl.createBuffer();
    const dstb = gl.createBuffer();
    gl.bindBuffer(gl.COPY_READ_BUFFER, src);
    gl.bufferData(gl.COPY_READ_BUFFER, new Uint8Array([10, 20, 30, 40, 50, 60, 70, 80]), gl.STATIC_DRAW);
    gl.bindBuffer(gl.COPY_WRITE_BUFFER, dstb);
    gl.bufferData(gl.COPY_WRITE_BUFFER, 8, gl.DYNAMIC_DRAW);
    gl.copyBufferSubData(gl.COPY_READ_BUFFER, gl.COPY_WRITE_BUFFER, 2, 4, 4);
    const cback = new Uint8Array(8);
    gl.getBufferSubData(gl.COPY_WRITE_BUFFER, 0, cback);
    assert(cback[4] === 30 && cback[5] === 40 && cback[6] === 50 && cback[7] === 60,
           'copyBufferSubData middle copy (got ' + Array.from(cback).join(',') + ')');
    assert(gl.getError() === gl.NO_ERROR, 'no error after copy');

    // =====================================================================
    // WebGL2 binding points
    // =====================================================================
    const ubo = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
    gl.bufferData(gl.UNIFORM_BUFFER, new Float32Array([1, 0, 0, 1]), gl.DYNAMIC_DRAW);
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, ubo);
    assert(gl.getError() === gl.NO_ERROR, 'UNIFORM_BUFFER bindBufferBase');
    const uback = new Float32Array(4);
    gl.getBufferSubData(gl.UNIFORM_BUFFER, 0, uback);
    eqArr(uback, new Float32Array([1, 0, 0, 1]), 'UBO round-trip');
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, null);

    // bindBufferRange (offset must respect UNIFORM_BUFFER_OFFSET_ALIGNMENT).
    // Note bindBufferBase(..., null) above also reset the generic
    // UNIFORM_BUFFER binding, so rebind before bufferData.
    const align = gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    assert(align >= 1, 'UNIFORM_BUFFER_OFFSET_ALIGNMENT queryable');
    gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
    gl.bufferData(gl.UNIFORM_BUFFER, align * 2, gl.DYNAMIC_DRAW);
    gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, ubo, align, 16);
    assert(gl.getError() === gl.NO_ERROR, 'bindBufferRange aligned offset');
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, null);

    const pp = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pp);
    gl.bufferData(gl.PIXEL_PACK_BUFFER, 64, gl.STREAM_READ);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    const pu = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, pu);
    gl.bufferData(gl.PIXEL_UNPACK_BUFFER, 64, gl.STREAM_DRAW);
    gl.bindBuffer(gl.PIXEL_UNPACK_BUFFER, null);
    assert(gl.getError() === gl.NO_ERROR, 'pixel pack/unpack binding points');

    // =====================================================================
    // Error cases
    // =====================================================================
    gl.bindBuffer(gl.ARRAY_BUFFER, b1);
    gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array(4), gl.STATIC_DRAW);
    gl.bufferSubData(gl.ARRAY_BUFFER, 2, new Uint8Array(8)); // writes past the end
    assert(gl.getError() === gl.INVALID_VALUE, 'bufferSubData past end -> INVALID_VALUE');
    assert(gl.getError() === gl.NO_ERROR, 'error cleared');

    // =====================================================================
    // Deletion safety
    // =====================================================================
    gl.deleteBuffer(b1);
    gl.deleteBuffer(b1);   // double delete is a no-op
    gl.deleteBuffer(null); // null is a no-op
    gl.deleteBuffer(src);
    gl.deleteBuffer(dstb);
    gl.deleteBuffer(ubo);
    gl.deleteBuffer(pp);
    gl.deleteBuffer(pu);
    assert(gl.getError() === gl.NO_ERROR, 'no error after deletes');

    console.log('webgl buffer tests passed');
}

document.body.removeChild(canvas);
