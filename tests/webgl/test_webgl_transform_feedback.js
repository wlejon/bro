// WebGL2 conformance subset — transform feedback: capture a varying into a
// buffer through the whole chain (transformFeedbackVaryings + relink, TF
// object, bindBufferBase, begin/draw/end, getBufferSubData) and assert the
// computed values numerically. Also: TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
// query, pause/resume, RASTERIZER_DISCARD, getTransformFeedbackVarying,
// getIndexedParameter rows, isTransformFeedback lifecycle, struct members
// captured by name.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const tfProbe = gl.createTransformFeedback();
    if (tfProbe === null) {
        // The backend reports transform feedback unsupported: a SKIP, so the
        // gap shows in every run's summary instead of passing untested.
        skipTest('createTransformFeedback() returned null: transform feedback unsupported');
    } else {

    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 1e-4); }

    // =====================================================================
    // Program: vOut = aIn * 2 + 1, captured via SEPARATE_ATTRIBS.
    // transformFeedbackVaryings takes effect on the NEXT link (GL semantics).
    // =====================================================================
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs,
        '#version 300 es\nin float aIn;\nout float vOut;\n' +
        'void main(){ vOut = aIn * 2.0 + 1.0; gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }');
    gl.compileShader(vs);
    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS))
        throw new Error('vs: ' + gl.getShaderInfoLog(vs));
    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs,
        '#version 300 es\nprecision highp float;\nout vec4 frag;\nvoid main(){ frag = vec4(1.0); }');
    gl.compileShader(fs);
    if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS))
        throw new Error('fs: ' + gl.getShaderInfoLog(fs));
    const prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs);
    gl.transformFeedbackVaryings(prog, ['vOut'], gl.SEPARATE_ATTRIBS);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS))
        throw new Error('link: ' + gl.getProgramInfoLog(prog));
    gl.useProgram(prog);

    assert(gl.getProgramParameter(prog, gl.TRANSFORM_FEEDBACK_VARYINGS) === 1,
           'TRANSFORM_FEEDBACK_VARYINGS count');
    assert(gl.getProgramParameter(prog, gl.TRANSFORM_FEEDBACK_BUFFER_MODE) === gl.SEPARATE_ATTRIBS,
           'TRANSFORM_FEEDBACK_BUFFER_MODE');
    const varying = gl.getTransformFeedbackVarying(prog, 0);
    assert(varying !== null && varying.name === 'vOut' && varying.type === gl.FLOAT &&
           varying.size === 1, 'getTransformFeedbackVarying metadata');
    assert(gl.getTransformFeedbackVarying(prog, 7) === null,
           'getTransformFeedbackVarying out of range -> null');
    assert(gl.getError() === gl.INVALID_VALUE,
           'getTransformFeedbackVarying out of range -> INVALID_VALUE');

    // =====================================================================
    // Input attribute + capture buffer
    // =====================================================================
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    const inBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, inBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([1, 2, 3, 4]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 1, gl.FLOAT, false, 0, 0);

    const tfBuf = gl.createBuffer();
    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, tfBuf);
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 16, gl.DYNAMIC_READ);
    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, null);

    // =====================================================================
    // TF object lifecycle + indexed binding
    // =====================================================================
    const tf = gl.createTransformFeedback();
    assert(gl.isTransformFeedback(tf) === false, 'isTransformFeedback false before bind');
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, tf);
    assert(gl.isTransformFeedback(tf) === true, 'isTransformFeedback true after bind');
    gl.bindBufferRange(gl.TRANSFORM_FEEDBACK_BUFFER, 0, tfBuf, 0, 16);

    assert(gl.getIndexedParameter(gl.TRANSFORM_FEEDBACK_BUFFER_BINDING, 0) === tfBuf,
           'getIndexedParameter(TRANSFORM_FEEDBACK_BUFFER_BINDING) identity');
    assert(gl.getIndexedParameter(gl.TRANSFORM_FEEDBACK_BUFFER_START, 0) === 0,
           'TRANSFORM_FEEDBACK_BUFFER_START');
    assert(gl.getIndexedParameter(gl.TRANSFORM_FEEDBACK_BUFFER_SIZE, 0) === 16,
           'TRANSFORM_FEEDBACK_BUFFER_SIZE');

    // =====================================================================
    // Keystone: capture 4 points, read the values back
    // =====================================================================
    const q = gl.createQuery();
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.beginQuery(gl.TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, q);
    gl.beginTransformFeedback(gl.POINTS);
    assert(gl.getParameter(gl.TRANSFORM_FEEDBACK_ACTIVE) === true, 'TF active during capture');
    gl.drawArrays(gl.POINTS, 0, 4);
    gl.endTransformFeedback();
    gl.endQuery(gl.TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    gl.disable(gl.RASTERIZER_DISCARD);
    assert(gl.getParameter(gl.TRANSFORM_FEEDBACK_ACTIVE) === false, 'TF inactive after end');

    const captured = new Float32Array(4);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, captured);
    for (let i = 0; i < 4; i++) {
        assert(near(captured[i], (i + 1) * 2 + 1),
               'captured[' + i + '] got ' + captured[i] + ' want ' + ((i + 1) * 2 + 1));
    }

    // Primitives-written query: 4 points = 4 primitives.
    gl.finish();
    const deadline = Date.now() + 5000;
    while (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) && Date.now() < deadline) {
        gl.flush();
    }
    const written = gl.getQueryParameter(q, gl.QUERY_RESULT);
    assert(written === 4, 'TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN === 4, got ' + written);
    assert(gl.getError() === gl.NO_ERROR, 'no error after keystone capture');

    // =====================================================================
    // pause/resume: capture 2, pause, resume, capture 2 more
    // =====================================================================
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 16, gl.DYNAMIC_READ); // reset via generic bind point
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.beginTransformFeedback(gl.POINTS);
    gl.drawArrays(gl.POINTS, 0, 2);
    gl.pauseTransformFeedback();
    assert(gl.getParameter(gl.TRANSFORM_FEEDBACK_PAUSED) === true, 'paused state visible');
    gl.resumeTransformFeedback();
    assert(gl.getParameter(gl.TRANSFORM_FEEDBACK_PAUSED) === false, 'resumed state visible');
    gl.drawArrays(gl.POINTS, 2, 2);
    gl.endTransformFeedback();
    gl.disable(gl.RASTERIZER_DISCARD);

    const captured2 = new Float32Array(4);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, captured2);
    for (let i = 0; i < 4; i++) {
        assert(near(captured2[i], (i + 1) * 2 + 1),
               'pause/resume captured[' + i + '] got ' + captured2[i]);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error after pause/resume capture');

    // =====================================================================
    // Interleaved capture of gl_Position (as the shader wrote it) and a
    // varying, instanced TRIANGLES: a trailing partial triangle is not
    // captured, records run instance by instance.
    // =====================================================================
    const vs2 = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs2,
        '#version 300 es\nin float aIn;\nout float vOut;\nflat out ivec2 vId;\n' +
        'void main(){ vOut = aIn * 2.0 + 1.0; vId = ivec2(gl_VertexID, gl_InstanceID);\n' +
        '  gl_Position = vec4(aIn, -aIn, 0.5, 1.0); }');
    gl.compileShader(vs2);
    const fs2 = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs2, '#version 300 es\nprecision highp float;\nflat in ivec2 vId;\nin float vOut;\nout vec4 o;\n' +
                    'void main(){ o = vec4(vOut, float(vId.x), 0.0, 1.0); }');
    gl.compileShader(fs2);
    const prog2 = gl.createProgram();
    gl.attachShader(prog2, vs2); gl.attachShader(prog2, fs2);
    gl.transformFeedbackVaryings(prog2, ['gl_Position', 'vOut', 'vId'], gl.INTERLEAVED_ATTRIBS);
    gl.linkProgram(prog2);
    assert(gl.getProgramParameter(prog2, gl.LINK_STATUS), 'interleaved program links: ' + gl.getProgramInfoLog(prog2));
    const v2 = gl.getTransformFeedbackVarying(prog2, 2);
    assert(v2 && v2.name === 'vId' && v2.type === gl.INT_VEC2 && v2.size === 1, 'ivec2 varying metadata');
    gl.useProgram(prog2);
    const words = 4 + 1 + 2;
    const tfBuf2 = gl.createBuffer();
    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, tfBuf2);
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 6 * words * 4 + 8, gl.DYNAMIC_READ);
    const tf2 = gl.createTransformFeedback();
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, tf2);
    gl.bindBufferRange(gl.TRANSFORM_FEEDBACK_BUFFER, 0, tfBuf2, 8, 6 * words * 4);
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.beginTransformFeedback(gl.TRIANGLES);
    gl.drawArrays(gl.POINTS, 0, 3);
    assert(gl.getError() === gl.INVALID_OPERATION, 'a draw in another primitive mode is INVALID_OPERATION');
    gl.useProgram(prog);
    assert(gl.getError() === gl.INVALID_OPERATION, 'useProgram while capturing is INVALID_OPERATION');
    const ibo = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2]), gl.STATIC_DRAW);
    gl.drawElements(gl.TRIANGLES, 3, gl.UNSIGNED_SHORT, 0);
    assert(gl.getError() === gl.INVALID_OPERATION, 'drawElements while capturing is INVALID_OPERATION');
    gl.drawArraysInstanced(gl.TRIANGLES, 0, 4, 2);  // 3 records per instance
    assert(gl.getError() === gl.NO_ERROR, 'instanced capture');
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    assert(gl.getError() === gl.INVALID_OPERATION, 'a capture past the bound range is INVALID_OPERATION');
    gl.endTransformFeedback();
    gl.disable(gl.RASTERIZER_DISCARD);
    const rec = new Float32Array(6 * words);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 8, rec);
    const recInt = new Int32Array(rec.buffer);
    let ok = true;
    for (let r = 0; r < 6; r++) {
        const inst = Math.floor(r / 3), v = r % 3, a = v + 1, o = r * words;
        ok = ok && near(rec[o], a) && near(rec[o + 1], -a) && near(rec[o + 2], 0.5) && near(rec[o + 3], 1) &&
             near(rec[o + 4], a * 2 + 1) && recInt[o + 5] === v && recInt[o + 6] === inst;
        if (!ok) { assert(false, 'record ' + r + ': ' + Array.from(rec.subarray(o, o + 5)) + ' ids ' + recInt[o + 5] + ',' + recInt[o + 6]); break; }
    }
    assert(ok, 'interleaved records: position, varying and ids per vertex and instance');
    const head = new Float32Array(2);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, head);
    assert(head[0] === 0 && head[1] === 0, 'nothing written before the bound offset');

    // The captured buffer feeds a draw: the positions as a vertex array.
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, null);
    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, tfBuf2);
    gl.vertexAttribPointer(0, 1, gl.FLOAT, false, words * 4, 8 + 4 * 4);
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, tfBuf);
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 16, gl.DYNAMIC_READ);
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.beginTransformFeedback(gl.POINTS);
    gl.drawArrays(gl.POINTS, 0, 4);
    gl.endTransformFeedback();
    gl.disable(gl.RASTERIZER_DISCARD);
    const chained = new Float32Array(4);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, chained);
    // vOut of the first pass (3, 5, 7, 3) through vOut = x * 2 + 1.
    assert(near(chained[0], 7) && near(chained[1], 11) && near(chained[2], 15) && near(chained[3], 7),
           'a second capture reads the first as its vertex array: ' + Array.from(chained));
    gl.bindBuffer(gl.ARRAY_BUFFER, inBuf);
    gl.vertexAttribPointer(0, 1, gl.FLOAT, false, 0, 0);
    gl.deleteTransformFeedback(tf2);
    assert(gl.getError() === gl.NO_ERROR, 'no error after the interleaved capture');

    // =====================================================================
    // Struct members, and an element of an array member, captured by name;
    // a whole struct, or a member it does not have, fails the link.
    // =====================================================================
    function structProgram(varyings) {
        const svs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(svs,
            '#version 300 es\nin float aIn;\n' +
            'struct S { float a; vec2 b; float list[3]; };\nout S s;\n' +
            'void main(){ s.a = aIn; s.b = vec2(aIn * 10.0, -aIn);\n' +
            '  s.list[0] = 0.0; s.list[1] = aIn + 100.0; s.list[2] = 0.0;\n' +
            '  gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }');
        gl.compileShader(svs);
        if (!gl.getShaderParameter(svs, gl.COMPILE_STATUS))
            throw new Error('struct vs: ' + gl.getShaderInfoLog(svs));
        const p = gl.createProgram();
        gl.attachShader(p, svs); gl.attachShader(p, fs);
        gl.bindAttribLocation(p, 0, 'aIn');
        gl.transformFeedbackVaryings(p, varyings, gl.INTERLEAVED_ATTRIBS);
        gl.linkProgram(p);
        return p;
    }
    assert(!gl.getProgramParameter(structProgram(['s']), gl.LINK_STATUS), 'a whole struct fails the link');
    assert(!gl.getProgramParameter(structProgram(['s.nope']), gl.LINK_STATUS), 'a missing member fails the link');
    const sprog = structProgram(['s.b', 's.list[1]', 's.a']);
    assert(gl.getProgramParameter(sprog, gl.LINK_STATUS), 'struct members link: ' + gl.getProgramInfoLog(sprog));
    const sv = gl.getTransformFeedbackVarying(sprog, 0);
    assert(sv && sv.name === 's.b' && sv.type === gl.FLOAT_VEC2 && sv.size === 1, 'the member\'s metadata');
    const sl = gl.getTransformFeedbackVarying(sprog, 1);
    assert(sl && sl.name === 's.list[1]' && sl.type === gl.FLOAT && sl.size === 1, 'the element\'s metadata');
    gl.useProgram(sprog);
    const tf3 = gl.createTransformFeedback();
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, tf3);
    const structBuf = gl.createBuffer();
    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, structBuf);
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 32, gl.DYNAMIC_READ);
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, structBuf);
    gl.enable(gl.RASTERIZER_DISCARD);
    gl.beginTransformFeedback(gl.POINTS);
    gl.drawArrays(gl.POINTS, 0, 2);
    gl.endTransformFeedback();
    gl.disable(gl.RASTERIZER_DISCARD);
    const members = new Float32Array(8);
    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, members);
    assert(Array.from(members).join() === '10,-1,101,1,20,-2,102,2',
           'struct members captured interleaved: ' + Array.from(members));
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, null);
    gl.deleteTransformFeedback(tf3);
    assert(gl.getError() === gl.NO_ERROR, 'no error after the struct capture');

    // =====================================================================
    // Deletion semantics
    // =====================================================================
    gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, null);
    gl.deleteTransformFeedback(tf);
    assert(gl.isTransformFeedback(tf) === false, 'isTransformFeedback false after delete');
    gl.deleteTransformFeedback(tf);   // double delete is a no-op
    gl.deleteTransformFeedback(null); // null is a no-op
    gl.deleteTransformFeedback(tfProbe);
    gl.deleteQuery(q);
    assert(gl.getError() === gl.NO_ERROR, 'no error after deletes');

    console.log('webgl transform feedback tests passed');
    }
}

document.body.removeChild(canvas);
