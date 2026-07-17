// WebGL2 object lifetime / GC churn — WebGL object wrappers (buffers,
// textures, shaders, programs, FBOs, RBOs, VAOs, uniform locations) are a
// QuickJS GC-hazard class. Create/delete churn plus dropped references, with
// periodic virtual-time GC sweeps; Debug builds assert on leaked JS values at
// teardown. Exercises the qjsbind wrappers in src/js/webgl2_bindings*.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '32');
canvas.setAttribute('height', '32');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    const vsSrc = '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }';
    const fsSrc = '#version 300 es\nprecision highp float;\nuniform vec4 uC;\nout vec4 frag;\n' +
                  'void main(){ frag = uC; }';

    for (let iter = 0; iter < 60; iter++) {
        // Buffers — half deleted explicitly, half dropped for GC
        const b1 = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, b1);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([iter, 1, 2, 3]), gl.STATIC_DRAW);
        gl.deleteBuffer(b1);
        gl.createBuffer(); // dropped

        // Textures
        const t1 = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      new Uint8Array([iter & 0xFF, 0, 0, 255]));
        gl.deleteTexture(t1); // delete while bound
        gl.createTexture();   // dropped

        // Shaders + programs, including uniform-location wrappers
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, vsSrc);
        gl.compileShader(vs);
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSrc);
        gl.compileShader(fs);
        const p = gl.createProgram();
        gl.attachShader(p, vs);
        gl.attachShader(p, fs);
        gl.linkProgram(p);
        gl.getUniformLocation(p, 'uC');       // dropped location wrapper
        const loc = gl.getUniformLocation(p, 'uC');
        gl.useProgram(p);
        gl.uniform4fv(loc, [0.1, 0.2, 0.3, 1]);
        gl.useProgram(null);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        if (iter % 2 === 0) gl.deleteProgram(p); // else dropped

        // FBO / RBO / VAO
        const f = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, f);
        const r = gl.createRenderbuffer();
        gl.bindRenderbuffer(gl.RENDERBUFFER, r);
        gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, 4, 4);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, r);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        const v = gl.createVertexArray();
        gl.bindVertexArray(v);
        gl.bindVertexArray(null);
        if (iter % 2 === 0) {
            gl.deleteFramebuffer(f);
            gl.deleteRenderbuffer(r);
            gl.deleteVertexArray(v);
        } // else dropped

        // Double deletes and null deletes are no-ops
        gl.deleteBuffer(b1);
        gl.deleteTexture(t1);
        gl.deleteBuffer(null);
        gl.deleteTexture(null);
        gl.deleteProgram(null);
        gl.deleteFramebuffer(null);
        gl.deleteRenderbuffer(null);
        gl.deleteVertexArray(null);

        // Periodic GC via virtual time (headless runs a GC sweep ~1/s)
        if (iter % 15 === 14) advanceTime(1200);
    }

    // Final sweep, then prove the context still works
    advanceTime(2500);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 32, 32);
    gl.clearColor(0, 0.25, 0.5, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    const b = new Uint8Array(4);
    gl.readPixels(16, 16, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
    assert(Math.abs(b[1] - 64) <= 3 && Math.abs(b[2] - 128) <= 3,
           'context alive after churn (got ' + Array.from(b).join(',') + ')');

    // Drain any errors accumulated by deleted-object edge cases
    let guard = 0;
    while (gl.getError() !== gl.NO_ERROR && guard++ < 16) {}
    assert(guard < 16, 'error queue drains');

    console.log('webgl gc churn tests passed');
}

document.body.removeChild(canvas);
