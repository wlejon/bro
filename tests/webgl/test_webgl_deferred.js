// WebGL2 calls the context's client encodes run on the backend later (at the
// next round trip, a flush or the frame's end), so their data is the client's
// copy, taken at call time: a typed array changed, dropped or collected after
// the call must not change what the call did. Covers uniformMatrix4fv,
// uniform4fv, bufferSubData (inline and a blob past 16 KiB), texSubImage2D, a
// ring that fills and wraps many times between round trips, and getError
// staying in API order across encoded and not-yet-encoded calls.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function makeProgram(vsSrc, fsSrc) {
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSrc); gl.compileShader(vs);
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) throw new Error('vs: ' + gl.getShaderInfoLog(vs));
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSrc); gl.compileShader(fs);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) throw new Error('fs: ' + gl.getShaderInfoLog(fs));
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error('link: ' + gl.getProgramInfoLog(p));
        return p;
    }
    function px(x, y) {
        const b = new Uint8Array(4);
        gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return b;
    }
    function assertPx(p, r, g, b, msg) {
        const ok = Math.abs(p[0] - r) <= 3 && Math.abs(p[1] - g) <= 3 && Math.abs(p[2] - b) <= 3;
        assert(ok, msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b].join(',') + ']');
    }
    // Make garbage and collect it, so a pointer into the heap kept past its
    // call would now read moved or freed memory.
    function churn() {
        let keep = 0;
        for (let i = 0; i < 2000; i++) keep += new Float32Array(64).length;
        __host.gc();
        return keep;
    }

    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nuniform mat4 uM;\n' +
        'void main(){ gl_Position = uM * vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\nout vec4 frag;\n' +
        'void main(){ frag = uColor; }');
    const uM = gl.getUniformLocation(prog, 'uM');
    const uColor = gl.getUniformLocation(prog, 'uColor');

    // A buffer of BIG floats (past the inline payload size), its first eight
    // the quad's corners.
    const BIG = 8192;
    const vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, BIG * 4, gl.DYNAMIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.viewport(0, 0, 64, 64);

    const LEFT = [-1, -1, 0, -1, -1, 1, 0, 1];   // the left half of the canvas
    const RIGHT = [0, -1, 1, -1, 0, 1, 1, 1];    // the right half
    const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

    function clearBlack() {
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }
    function draw() {
        gl.useProgram(prog);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    }

    // --- uniformMatrix4fv / uniform4fv: the arrays changed after the call ---
    {
        const verts = new Float32Array(LEFT);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, verts);
        clearBlack();
        gl.useProgram(prog);
        const m = new Float32Array(identity);
        gl.uniformMatrix4fv(uM, false, m);
        m.fill(0);  // a zero matrix would draw nothing
        const color = new Float32Array([0, 1, 0, 1]);
        gl.uniform4fv(uColor, color);
        color.set([1, 0, 0, 1]);
        draw();
        churn();
        assertPx(px(16, 32), 0, 255, 0, 'uniforms are the values at call time');
        assertPx(px(48, 32), 0, 0, 0, 'right half untouched');
    }

    // --- bufferSubData: inline, changed after the call; and a blob, dropped
    //     and collected before the replay ---
    {
        clearBlack();
        const small = new Float32Array(RIGHT);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, small);
        small.set(LEFT);
        draw();
        assertPx(px(48, 32), 0, 255, 0, 'inline bufferSubData is the data at call time (right)');
        assertPx(px(16, 32), 0, 0, 0, 'inline bufferSubData is the data at call time (left clear)');

        clearBlack();
        (() => {
            const big = new Float32Array(BIG);
            big.set(LEFT);
            gl.bufferSubData(gl.ARRAY_BUFFER, 0, big);  // 32 KiB: a blob
            big.set(RIGHT);
        })();
        churn();
        draw();
        assertPx(px(16, 32), 0, 255, 0, 'blob bufferSubData survives its array (left)');
        assertPx(px(48, 32), 0, 0, 0, 'blob bufferSubData survives its array (right clear)');
    }

    // --- texSubImage2D after encoded calls, its data changed after ---
    {
        const tprog = makeProgram(
            '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
            'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }',
            '#version 300 es\nprecision highp float;\nuniform sampler2D uTex;\nin vec2 vUv;\nout vec4 frag;\n' +
            'void main(){ frag = texture(uTex, vUv); }');
        const tex = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]));
        clearBlack();
        gl.useProgram(tprog);
        gl.uniform1i(gl.getUniformLocation(tprog, 'uTex'), 0);
        const texel = new Uint8Array([0, 0, 255, 255]);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, texel);
        texel.set([255, 0, 0, 255]);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        churn();
        assertPx(px(32, 32), 0, 0, 255, 'texSubImage2D is the data at call time');
        gl.deleteTexture(tex);
        gl.deleteProgram(tprog);
    }

    // --- Many calls between round trips: the ring fills and wraps; one array
    //     reused for every call, as three.js does, blobs on the way ---
    {
        clearBlack();
        gl.useProgram(prog);
        const m = new Float32Array(16);
        const big = new Float32Array(BIG);
        for (let i = 0; i < 40000; i++) {
            m.set(identity);
            m[0] = m[5] = (i % 7) / 7;      // scales that are not the last
            gl.uniformMatrix4fv(uM, false, m);
            gl.uniform4f(uColor, 1, 0, 1, 1);
            if (i % 500 === 0) {
                big.set(i % 1000 === 0 ? RIGHT : LEFT);
                gl.bufferSubData(gl.ARRAY_BUFFER, 0, big);
            }
        }
        m.set(identity);
        gl.uniformMatrix4fv(uM, false, m);
        m.fill(0);
        big.set(LEFT);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, big);
        big.set(RIGHT);
        gl.uniform4fv(uColor, [1, 1, 0, 1]);
        draw();
        churn();
        assertPx(px(16, 32), 255, 255, 0, 'after a wrapped ring, the last calls win (left)');
        assertPx(px(48, 32), 0, 0, 0, 'after a wrapped ring, the last calls win (right clear)');
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error so far');

    // --- getError: the first error in API order, encoded or not ---
    {
        gl.viewport(0, 0, -1, -1);                         // encoded: INVALID_VALUE
        gl.texParameteri(0xBAD, gl.TEXTURE_MIN_FILTER, 0);  // encoded too: INVALID_ENUM
        assert(gl.getError() === gl.INVALID_VALUE, 'the first encoded error came first');
        assert(gl.getError() === gl.NO_ERROR, 'one error at a time');

        gl.texParameteri(0xBAD, gl.TEXTURE_MIN_FILTER, 0);  // INVALID_ENUM, first now
        gl.viewport(0, 0, -1, -1);
        assert(gl.getError() === gl.INVALID_ENUM, 'the texture error came first');

        gl.createShader(0xBAD);                             // the client's own: INVALID_ENUM
        gl.viewport(0, 0, -1, -1);
        assert(gl.getError() === gl.INVALID_ENUM, 'the client error came first');
        assert(gl.getError() === gl.NO_ERROR, 'one error at a time (client)');
        assert(gl.getError() === gl.NO_ERROR, 'one error at a time (2)');

        gl.enable(0xBAD);
        assert(gl.isEnabled(gl.BLEND) === false, 'isEnabled after an encoded error');
        assert(gl.getError() === gl.INVALID_ENUM, 'an encoded error survives a round trip');
        gl.viewport(0, 0, 64, 64);
    }

    // --- Into a framebuffer: an upload from a collected array, queued draws,
    //     then readPixels; and getUniformLocation straight after linkProgram ---
    {
        const tprog = makeProgram(
            '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
            'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }',
            '#version 300 es\nprecision highp float;\nuniform sampler2D uTex;\nin vec2 vUv;\nout vec4 frag;\n' +
            'void main(){ frag = texture(uTex, vUv); }');
        const uTex = gl.getUniformLocation(tprog, 'uTex');  // no LINK_STATUS read first
        assert(uTex !== null, 'getUniformLocation straight after linkProgram');
        const src = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, src);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        (() => {
            const data = new Uint8Array(32 * 32 * 4);  // 16 KiB: a blob
            for (let i = 0; i < data.length; i += 4) data.set([0, 255, 255, 255], i);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 32, 32, 0, gl.RGBA, gl.UNSIGNED_BYTE, data);
            data.fill(0);
        })();
        churn();
        const target = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, target);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 64, 64);
        const fb = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]));
        clearBlack();
        gl.useProgram(tprog);
        gl.uniform1i(uTex, 0);
        gl.bindTexture(gl.TEXTURE_2D, src);
        gl.bindVertexArray(vao);
        for (let i = 0; i < 50; i++) gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        assertPx(px(10, 50), 0, 255, 255, 'readPixels after queued draws into a framebuffer');
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.deleteFramebuffer(fb);
        gl.deleteTexture(target);
        gl.deleteTexture(src);
        gl.deleteProgram(tprog);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array(LEFT));
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error from the framebuffer section');

    // --- Loss with commands pending, then calls while lost ---
    {
        const ext = gl.getExtension('WEBGL_lose_context');
        gl.uniformMatrix4fv(uM, false, new Float32Array(identity));
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array(BIG));
        draw();
        ext.loseContext();
        gl.uniformMatrix4fv(uM, false, new Float32Array(identity));
        draw();
        assert(gl.isContextLost(), 'lost');
        assert(gl.getError() === gl.CONTEXT_LOST_WEBGL, 'lost error');
    }

    console.log('webgl deferred-command tests passed');
}
