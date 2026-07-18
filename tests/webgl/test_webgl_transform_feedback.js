// WebGL2 conformance subset — transform feedback: capture a varying into a
// buffer through the whole chain (transformFeedbackVaryings + relink, TF
// object, bindBufferBase, begin/draw/end, getBufferSubData) and assert the
// computed values numerically. Also: TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN
// query, pause/resume, RASTERIZER_DISCARD, getTransformFeedbackVarying,
// getIndexedParameter rows, isTransformFeedback lifecycle.
// Exercises src/js/webgl2_bindings_objects.cpp + src/webgl/webgl2_context.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    const tfProbe = gl.createTransformFeedback();
    if (tfProbe === null) {
        // Driver lacks ARB_transform_feedback2 (TF objects). Documented gap.
        console.log('no transform feedback objects; skipping');
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
    assert(gl.getQueryParameter(q, gl.QUERY_RESULT) === 4,
           'TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN === 4');
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
