// WebGL2 conformance subset — sampler objects: create/bind/parameter
// round-trips, sampler state overriding texture state (proven via pixels),
// unbind restores texture state, deletion semantics, isSampler lifecycle.
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

    // =====================================================================
    // Parameter round-trips (int + float pnames)
    // =====================================================================
    const s = gl.createSampler();
    assert(s !== null, 'createSampler');

    gl.samplerParameteri(s, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.samplerParameteri(s, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.samplerParameteri(s, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.samplerParameteri(s, gl.TEXTURE_WRAP_T, gl.MIRRORED_REPEAT);
    gl.samplerParameterf(s, gl.TEXTURE_MIN_LOD, -500.5);
    gl.samplerParameterf(s, gl.TEXTURE_MAX_LOD, 250.25);
    assert(gl.getSamplerParameter(s, gl.TEXTURE_MIN_FILTER) === gl.NEAREST, 'MIN_FILTER round-trip');
    assert(gl.getSamplerParameter(s, gl.TEXTURE_MAG_FILTER) === gl.NEAREST, 'MAG_FILTER round-trip');
    assert(gl.getSamplerParameter(s, gl.TEXTURE_WRAP_S) === gl.REPEAT, 'WRAP_S round-trip');
    assert(gl.getSamplerParameter(s, gl.TEXTURE_WRAP_T) === gl.MIRRORED_REPEAT, 'WRAP_T round-trip');
    assert(near(gl.getSamplerParameter(s, gl.TEXTURE_MIN_LOD), -500.5, 0.01), 'MIN_LOD float round-trip');
    assert(near(gl.getSamplerParameter(s, gl.TEXTURE_MAX_LOD), 250.25, 0.01), 'MAX_LOD float round-trip');
    assert(gl.getError() === gl.NO_ERROR, 'no error after parameter round-trips');

    // =====================================================================
    // Pixel proof: a bound sampler's wrap mode overrides the texture's.
    // Texture: 2x1, [red | blue], WRAP_S = CLAMP_TO_EDGE on the texture.
    // Fragment samples at u = 1.25: CLAMP -> blue (right edge), the
    // sampler's REPEAT -> u wraps to 0.25 -> red.
    // =====================================================================
    const prog = makeProgram(
        '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nuniform sampler2D uTex;\nout vec4 frag;\n' +
        'void main(){ frag = texture(uTex, vec2(1.25, 0.5)); }');
    gl.useProgram(prog);
    gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    const tex = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 2, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([255, 0, 0, 255,  0, 0, 255, 255]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    function draw() { gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }

    // No sampler bound: texture's CLAMP_TO_EDGE governs -> blue.
    draw();
    assertPx(px(32, 32), 0, 0, 255, 'no sampler: texture CLAMP_TO_EDGE');

    // Sampler bound to unit 0: its REPEAT overrides -> red.
    gl.bindSampler(0, s);
    draw();
    assertPx(px(32, 32), 255, 0, 0, 'sampler REPEAT overrides texture wrap');

    // Unbind sampler: texture state governs again -> blue.
    gl.bindSampler(0, null);
    draw();
    assertPx(px(32, 32), 0, 0, 255, 'unbind sampler restores texture wrap');
    assert(gl.getError() === gl.NO_ERROR, 'no error in sampler override pass');

    // =====================================================================
    // getParameter(SAMPLER_BINDING) is a binding-object query (documented
    // to return null in this implementation) — must not raise an error.
    // =====================================================================
    gl.getParameter(gl.SAMPLER_BINDING);
    assert(gl.getError() === gl.NO_ERROR, 'SAMPLER_BINDING query is error-free');

    // =====================================================================
    // isSampler lifecycle + deletion semantics
    // =====================================================================
    // Unlike buffers/textures, GenSamplers creates the object immediately
    // (ES 3.0 semantics, matched by desktop drivers) — true before any bind.
    const s2 = gl.createSampler();
    assert(gl.isSampler(s2) === true, 'isSampler true straight after create');
    gl.bindSampler(1, s2);
    assert(gl.isSampler(s2) === true, 'isSampler true after bind');
    assert(gl.isSampler(null) === false, 'isSampler(null) false');

    // Deleting a bound sampler unbinds it (back to texture state -> blue).
    gl.bindSampler(0, s);
    draw();
    assertPx(px(32, 32), 255, 0, 0, 'sampler active before delete');
    gl.deleteSampler(s);
    draw();
    assertPx(px(32, 32), 0, 0, 255, 'deleting bound sampler unbinds it');
    assert(gl.isSampler(s) === false, 'isSampler false after delete');
    gl.deleteSampler(s);    // double delete is a no-op
    gl.deleteSampler(null); // null is a no-op
    gl.deleteSampler(s2);
    assert(gl.getError() === gl.NO_ERROR, 'no error after deletes');

    console.log('webgl sampler tests passed');
}

document.body.removeChild(canvas);
