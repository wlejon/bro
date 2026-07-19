// WebGL2 conformance subset — query objects: occlusion queries with
// ANY_SAMPLES_PASSED (positive + scissored-out negative), the conservative
// target, getQuery(CURRENT_QUERY) object identity, non-stalling
// QUERY_RESULT_AVAILABLE polling bounded by finish() + a wall deadline,
// isQuery lifecycle, error paths.
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

    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nout vec4 frag;\n' +
        'void main(){ frag = vec4(1.0, 0.0, 0.0, 1.0); }');
    gl.useProgram(prog);
    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    // Deterministic result readout: finish() forces completion, then poll
    // QUERY_RESULT_AVAILABLE (which must never stall) under a wall deadline.
    function queryResult(q) {
        gl.finish();
        const deadline = Date.now() + 5000;
        while (!gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) && Date.now() < deadline) {
            gl.flush();
        }
        assert(gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) === true,
               'query result became available');
        return gl.getQueryParameter(q, gl.QUERY_RESULT);
    }

    // =====================================================================
    // Occlusion query: fullscreen draw -> samples pass
    // =====================================================================
    const q1 = gl.createQuery();
    assert(q1 !== null, 'createQuery');
    assert(gl.isQuery(q1) === false, 'isQuery false before first beginQuery');

    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.beginQuery(gl.ANY_SAMPLES_PASSED, q1);
    assert(gl.isQuery(q1) === true, 'isQuery true once begun');
    // getQuery returns the exact active query object.
    assert(gl.getQuery(gl.ANY_SAMPLES_PASSED, gl.CURRENT_QUERY) === q1,
           'getQuery(CURRENT_QUERY) identity while active');
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.endQuery(gl.ANY_SAMPLES_PASSED);
    assert(gl.getQuery(gl.ANY_SAMPLES_PASSED, gl.CURRENT_QUERY) === null,
           'getQuery(CURRENT_QUERY) null after endQuery');
    assert(queryResult(q1) !== 0, 'fullscreen draw passes samples');

    // =====================================================================
    // Occlusion query: fully scissored-out draw -> no samples pass
    //
    // Skipped on Mesa's d3d12 driver (WSL2/WSLg): an occlusion query followed
    // by glFinish makes it stop honoring a subsequent ZERO-AREA scissor rect,
    // so the draw really is rasterized and the query is telling the truth.
    // Isolated: query + finish() + scissor(0,0,0,0) leaves the center pixel
    // drawn; drop either the query or the finish() and it clips correctly. A
    // non-empty scissor is still honored, so the empty rect is being read as
    // "no scissor" rather than "clip everything". queryResult() below calls
    // finish(), which is what arms it. Passes under LIBGL_ALWAYS_SOFTWARE=1.
    // =====================================================================
    const renderer = String(gl.getParameter(gl.RENDERER));
    const emptyScissorBroken = /D3D12/i.test(renderer);
    let q2 = null;   // stays null when skipped; deleteQuery(null) is a no-op
    if (emptyScissorBroken) {
        console.log('skipping scissored-out occlusion query on ' + renderer +
                    ' (driver ignores zero-area scissor after a query + finish)');
    } else {
        q2 = gl.createQuery();
        gl.enable(gl.SCISSOR_TEST);
        gl.scissor(0, 0, 0, 0);
        gl.beginQuery(gl.ANY_SAMPLES_PASSED, q2);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        gl.endQuery(gl.ANY_SAMPLES_PASSED);
        gl.disable(gl.SCISSOR_TEST);
        gl.scissor(0, 0, 64, 64);
        assert(queryResult(q2) === 0, 'scissored-out draw passes no samples');
    }

    // =====================================================================
    // Conservative target: only the positive case is spec-guaranteed
    // (false positives are allowed, false negatives are not).
    // =====================================================================
    const q3 = gl.createQuery();
    gl.beginQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE, q3);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.endQuery(gl.ANY_SAMPLES_PASSED_CONSERVATIVE);
    assert(queryResult(q3) !== 0, 'conservative query sees the visible draw');
    assert(gl.getError() === gl.NO_ERROR, 'no error after conservative query');

    // =====================================================================
    // Error paths + deletion semantics
    // =====================================================================
    const qDead = gl.createQuery();
    gl.deleteQuery(qDead);
    assert(gl.isQuery(qDead) === false, 'isQuery false after delete');
    gl.beginQuery(gl.ANY_SAMPLES_PASSED, qDead);
    assert(gl.getError() === gl.INVALID_OPERATION, 'beginQuery on deleted query -> INVALID_OPERATION');
    gl.getQueryParameter(qDead, gl.QUERY_RESULT_AVAILABLE);
    assert(gl.getError() === gl.INVALID_OPERATION, 'getQueryParameter on deleted query -> INVALID_OPERATION');

    gl.deleteQuery(q1);
    gl.deleteQuery(q1);   // double delete is a no-op
    gl.deleteQuery(null); // null is a no-op
    gl.deleteQuery(q2);
    gl.deleteQuery(q3);
    assert(gl.getError() === gl.NO_ERROR, 'no error after deletes');

    console.log('webgl query tests passed');
}

document.body.removeChild(canvas);
