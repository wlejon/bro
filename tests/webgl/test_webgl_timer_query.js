// EXT_disjoint_timer_query_webgl2 on Vulkan timestamps: advertised and
// enabled through getExtension, TIME_ELAPSED_EXT through beginQuery /
// endQuery, TIMESTAMP_EXT through queryCounterEXT, QUERY_COUNTER_BITS_EXT,
// GPU_DISJOINT_EXT and getParameter(TIMESTAMP_EXT). Results are polled the
// way an app does — QUERY_RESULT_AVAILABLE on later frames — and become
// available without anything waiting on the GPU.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const TIME_ELAPSED_EXT = 0x88BF, TIMESTAMP_EXT = 0x8E28, GPU_DISJOINT_EXT = 0x8FBB;
    const supported = gl.getSupportedExtensions().includes('EXT_disjoint_timer_query_webgl2');

    // Before the extension is enabled its enums are not enums.
    const early = gl.createQuery();
    gl.beginQuery(TIME_ELAPSED_EXT, early);
    assert(gl.getError() === gl.INVALID_ENUM, 'TIME_ELAPSED_EXT is INVALID_ENUM before getExtension');
    gl.getParameter(GPU_DISJOINT_EXT);
    assert(gl.getError() === gl.INVALID_ENUM, 'GPU_DISJOINT_EXT is INVALID_ENUM before getExtension');
    gl.deleteQuery(early);

    const ext = gl.getExtension('EXT_disjoint_timer_query_webgl2');
    if (!supported) {
        assert(ext === null, 'getExtension is null where the queue cannot timestamp');
        skipTest('the graphics queue has no timestamps');
    } else {
        assert(ext !== null, 'getExtension returns the extension it advertises');
        assert(gl.getExtension('EXT_disjoint_timer_query_webgl2') === ext, 'the same object every time');
        assert(ext.TIME_ELAPSED_EXT === TIME_ELAPSED_EXT && ext.TIMESTAMP_EXT === TIMESTAMP_EXT &&
               ext.GPU_DISJOINT_EXT === GPU_DISJOINT_EXT && ext.QUERY_COUNTER_BITS_EXT === 0x8864,
               'extension constants');
        assert(typeof ext.queryCounterEXT === 'function', 'queryCounterEXT');

        const bits = gl.getQuery(TIME_ELAPSED_EXT, ext.QUERY_COUNTER_BITS_EXT);
        assert(bits > 0 && bits <= 64, 'QUERY_COUNTER_BITS_EXT for TIME_ELAPSED_EXT: ' + bits);
        assert(gl.getQuery(TIMESTAMP_EXT, ext.QUERY_COUNTER_BITS_EXT) === bits, 'TIMESTAMP_EXT bits');
        assert(gl.getParameter(GPU_DISJOINT_EXT) === false, 'GPU_DISJOINT_EXT is false');
        assert(gl.getError() === gl.NO_ERROR, 'no error from the extension getters');

        // Work to time: many fullscreen fragments.
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, `#version 300 es
            void main() {
                vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
                gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
            }`);
        gl.compileShader(vs);
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, `#version 300 es
            precision highp float;
            out vec4 o;
            void main() {
                float a = gl_FragCoord.x;
                for (int i = 0; i < 64; i++) a = sin(a) * 1.0001 + 0.1;
                o = vec4(fract(a), 0.0, 0.0, 1.0);
            }`);
        gl.compileShader(fs);
        const prog = gl.createProgram();
        gl.attachShader(prog, vs);
        gl.attachShader(prog, fs);
        gl.linkProgram(prog);
        assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'timing program links');
        gl.useProgram(prog);
        gl.bindVertexArray(gl.createVertexArray());

        // --- TIME_ELAPSED_EXT -------------------------------------------------
        const q = gl.createQuery();
        gl.beginQuery(TIME_ELAPSED_EXT, q);
        assert(gl.getQuery(TIME_ELAPSED_EXT, gl.CURRENT_QUERY) === q, 'CURRENT_QUERY while active');
        gl.beginQuery(TIME_ELAPSED_EXT, gl.createQuery());
        assert(gl.getError() === gl.INVALID_OPERATION, 'a second active TIME_ELAPSED_EXT query is refused');
        for (let i = 0; i < 20; i++) gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.endQuery(TIME_ELAPSED_EXT);
        assert(gl.getQuery(TIME_ELAPSED_EXT, gl.CURRENT_QUERY) === null, 'no CURRENT_QUERY after endQuery');
        assert(gl.isQuery(q), 'isQuery once begun');
        assert(gl.getQueryParameter(q, gl.QUERY_RESULT_AVAILABLE) === false,
               'not available in the turn that recorded it');

        // An empty query, begun and ended with nothing between.
        const empty = gl.createQuery();
        gl.beginQuery(TIME_ELAPSED_EXT, empty);
        gl.endQuery(TIME_ELAPSED_EXT);

        // --- TIMESTAMP_EXT ----------------------------------------------------
        const t0 = gl.createQuery(), t1 = gl.createQuery();
        ext.queryCounterEXT(t0, TIMESTAMP_EXT);
        for (let i = 0; i < 20; i++) gl.drawArrays(gl.TRIANGLES, 0, 3);
        ext.queryCounterEXT(t1, TIMESTAMP_EXT);
        assert(gl.getError() === gl.NO_ERROR, 'queryCounterEXT records');
        ext.queryCounterEXT(t0, TIME_ELAPSED_EXT);
        assert(gl.getError() === gl.INVALID_ENUM, 'queryCounterEXT takes only TIMESTAMP_EXT');
        gl.beginQuery(TIME_ELAPSED_EXT, t0);
        assert(gl.getError() === gl.INVALID_OPERATION, 'a TIMESTAMP_EXT query cannot become TIME_ELAPSED_EXT');

        // --- poll on later frames, as an app would ----------------------------
        // (On frames that take real time: a slow GPU, the runners'
        // paravirtual one, does not finish within 200 frames a fraction of
        // a millisecond each.)
        let frames = 0;
        const until = perf.now() + 5000;
        const ready = (x) => gl.getQueryParameter(x, gl.QUERY_RESULT_AVAILABLE);
        while (!(ready(q) && ready(empty) && ready(t0) && ready(t1)) && (frames < 200 || perf.now() < until)) {
            advanceTime(16);
            if (frames >= 10) wallSleep(2);
            frames++;
        }
        assert(ready(q) && ready(empty) && ready(t0) && ready(t1),
               'results become available on later frames (' + frames + ' polled)');
        const elapsed = gl.getQueryParameter(q, gl.QUERY_RESULT);
        const none = gl.getQueryParameter(empty, gl.QUERY_RESULT);
        const s0 = gl.getQueryParameter(t0, gl.QUERY_RESULT), s1 = gl.getQueryParameter(t1, gl.QUERY_RESULT);
        const minElapsed = s1 > s0 ? 1 : 0;
        assert(typeof elapsed === 'number' && elapsed >= minElapsed && elapsed < 5e9,
               'TIME_ELAPSED_EXT measured the draws: ' + elapsed + ' ns');
        assert(none >= 0 && none <= elapsed, 'an empty query takes no longer than the draws: ' + none + ' ns');
        assert(s0 > 0 && s1 >= s0, 'timestamps increase: ' + s0 + ' -> ' + s1);
        assert(s1 - s0 < 5e9, 'timestamp difference is plausible: ' + (s1 - s0) + ' ns');
        assert(gl.getQueryParameter(q, gl.QUERY_RESULT) === elapsed, 'a result reads the same twice');

        // getParameter(TIMESTAMP_EXT): the GPU clock now, after the stamps.
        const nowNs = gl.getParameter(TIMESTAMP_EXT);
        assert(typeof nowNs === 'number' && nowNs >= s1, 'TIMESTAMP_EXT now (' + nowNs + ') is after t1 (' + s1 + ')');

        // QUERY_RESULT without polling still answers (waiting for its own work).
        const q2 = gl.createQuery();
        gl.beginQuery(TIME_ELAPSED_EXT, q2);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.endQuery(TIME_ELAPSED_EXT);
        assert(gl.getQueryParameter(q2, gl.QUERY_RESULT) >= 0, 'QUERY_RESULT right away');

        // Re-running a query replaces its result; deleting an active one ends it.
        gl.beginQuery(TIME_ELAPSED_EXT, q);
        gl.deleteQuery(q);
        assert(gl.getQuery(TIME_ELAPSED_EXT, gl.CURRENT_QUERY) === null, 'deleting the active query ends it');

        // Many queries in flight recycle their slots.
        for (let frame = 0; frame < 6; frame++) {
            const qs = [];
            for (let i = 0; i < 40; i++) {
                const x = gl.createQuery();
                gl.beginQuery(TIME_ELAPSED_EXT, x);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
                gl.endQuery(TIME_ELAPSED_EXT);
                qs.push(x);
            }
            advanceTime(16);
            advanceTime(16);
            for (const x of qs) {
                gl.getQueryParameter(x, gl.QUERY_RESULT);
                gl.deleteQuery(x);
            }
        }
        assert(gl.getError() === gl.NO_ERROR, 'no GL error');
    }
}
