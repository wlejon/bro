// WebGL command-stream ordering: state that changes between draws recorded
// in the same frame must reach each draw as it was when that draw was issued.
// Every check records all of its draws first and reads pixels only at the end,
// so nothing flushes or waits in between — the way a real frame runs.
// Covers per-draw descriptor sets (textures, uniform blocks), the default
// uniform block (> 128 bytes), bufferSubData / texImage2D / vertexAttrib*
// between draws, 8-bit index widening, many linked programs, and real fences.
// Exercises src/webgl/vulkan (command stream, draw, buffers, textures).

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
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) throw new Error('vs: ' + gl.getShaderInfoLog(vs));
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, fsSrc); gl.compileShader(fs);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) throw new Error('fs: ' + gl.getShaderInfoLog(fs));
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error('link: ' + gl.getProgramInfoLog(p));
        gl.deleteShader(vs); gl.deleteShader(fs);
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
    // 8 columns of 8x64 px: column i is viewport (i*8, 0, 8, 64).
    function column(i) { gl.viewport(i * 8, 0, 8, 64); }
    function colPx(i) { return px(i * 8 + 4, 32); }

    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const quadVao = gl.createVertexArray();
    gl.bindVertexArray(quadVao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const kVs = '#version 300 es\nin vec2 aPos;\nout vec2 vUv;\n' +
                'void main(){ vUv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }';

    function reset() {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, 64, 64);
        gl.clearColor(0, 0, 0, 1);
        gl.clear(gl.COLOR_BUFFER_BIT);
    }
    const palette = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0],
                     [255, 0, 255], [0, 255, 255], [255, 128, 0], [128, 0, 255]];

    // =====================================================================
    // A different texture per draw: each draw gets its own descriptor set.
    // =====================================================================
    {
        reset();
        const prog = makeProgram(kVs, '#version 300 es\nprecision highp float;\nin vec2 vUv;\n' +
            'uniform sampler2D uTex;\nout vec4 frag;\nvoid main(){ frag = texture(uTex, vUv); }');
        gl.useProgram(prog);
        gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0);
        const texes = palette.map(c => {
            const t = gl.createTexture();
            gl.bindTexture(gl.TEXTURE_2D, t);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                          new Uint8Array([c[0], c[1], c[2], 255]));
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            return t;
        });
        gl.bindVertexArray(quadVao);
        for (let i = 0; i < 8; ++i) {
            column(i);
            gl.bindTexture(gl.TEXTURE_2D, texes[i]);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'texture per draw, column ' + i);
    }

    // =====================================================================
    // A different uniform buffer per draw, and a default uniform block well
    // past 128 bytes (vec4[16] = 256 bytes) indexed per draw.
    // =====================================================================
    {
        reset();
        const prog = makeProgram(kVs, '#version 300 es\nprecision highp float;\n' +
            'layout(std140) uniform Tint { vec4 tint; };\nuniform vec4 uTable[16];\nuniform int uIndex;\n' +
            'out vec4 frag;\nvoid main(){ frag = tint * uTable[uIndex]; }');
        gl.useProgram(prog);
        gl.uniformBlockBinding(prog, gl.getUniformBlockIndex(prog, 'Tint'), 0);
        const table = new Float32Array(64);
        for (let i = 0; i < 16; ++i) table.set([1, 1, 1, 1], i * 4);
        gl.uniform4fv(gl.getUniformLocation(prog, 'uTable'), table);
        const uIndex = gl.getUniformLocation(prog, 'uIndex');
        const ubos = palette.map(c => {
            const b = gl.createBuffer();
            gl.bindBuffer(gl.UNIFORM_BUFFER, b);
            gl.bufferData(gl.UNIFORM_BUFFER, new Float32Array([c[0] / 255, c[1] / 255, c[2] / 255, 1]), gl.STATIC_DRAW);
            return b;
        });
        gl.bindVertexArray(quadVao);
        for (let i = 0; i < 8; ++i) {
            column(i);
            gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, ubos[i]);
            // Entry 8+i is the only white one; the rest of the table goes black.
            const t = new Float32Array(64);
            t.set([1, 1, 1, 1], (8 + i) * 4);
            gl.uniform4fv(gl.getUniformLocation(prog, 'uTable'), t);
            gl.uniform1i(uIndex, 8 + i);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'uniform block + table per draw, column ' + i);
    }

    // =====================================================================
    // bufferSubData between draws reaches only the draws after it.
    // =====================================================================
    const colorProg = makeProgram(
        '#version 300 es\nin vec2 aPos;\nin vec4 aColor;\nout vec4 vColor;\n' +
        'void main(){ vColor = aColor; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec4 vColor;\nout vec4 frag;\nvoid main(){ frag = vColor; }');
    gl.bindAttribLocation(colorProg, 0, 'aPos');
    {
        reset();
        gl.useProgram(colorProg);
        const aColor = gl.getAttribLocation(colorProg, 'aColor');
        const colors = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, colors);
        gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(16), gl.DYNAMIC_DRAW);
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, quad);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ARRAY_BUFFER, colors);
        gl.enableVertexAttribArray(aColor);
        gl.vertexAttribPointer(aColor, 4, gl.FLOAT, false, 0, 0);
        for (let i = 0; i < 8; ++i) {
            const c = palette[i];
            const v = [c[0] / 255, c[1] / 255, c[2] / 255, 1];
            gl.bindBuffer(gl.ARRAY_BUFFER, colors);
            gl.bufferSubData(gl.ARRAY_BUFFER, 0, new Float32Array([...v, ...v, ...v, ...v]));
            column(i);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'bufferSubData between draws, column ' + i);

        // Same again through bufferData re-specification at the same size.
        reset();
        for (let i = 0; i < 8; ++i) {
            const c = palette[7 - i];
            const v = [c[0] / 255, c[1] / 255, c[2] / 255, 1];
            gl.bindBuffer(gl.ARRAY_BUFFER, colors);
            gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([...v, ...v, ...v, ...v]), gl.DYNAMIC_DRAW);
            column(i);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[7 - i], 'bufferData between draws, column ' + i);

        // A disabled array's constant (vertexAttrib4f) between draws.
        reset();
        gl.disableVertexAttribArray(aColor);
        for (let i = 0; i < 8; ++i) {
            const c = palette[i];
            gl.vertexAttrib4f(aColor, c[0] / 255, c[1] / 255, c[2] / 255, 1);
            column(i);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'vertexAttrib4f between draws, column ' + i);
        gl.bindVertexArray(null);
    }

    // =====================================================================
    // texImage2D re-upload (same size) between draws.
    // =====================================================================
    {
        reset();
        const prog = makeProgram(kVs, '#version 300 es\nprecision highp float;\nin vec2 vUv;\n' +
            'uniform sampler2D uTex;\nout vec4 frag;\nvoid main(){ frag = texture(uTex, vUv); }');
        gl.useProgram(prog);
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.bindVertexArray(quadVao);
        for (let i = 0; i < 8; ++i) {
            const c = palette[i];
            const data = new Uint8Array(16);
            for (let k = 0; k < 4; ++k) data.set([c[0], c[1], c[2], 255], k * 4);
            if (i % 2 === 0) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, data);
            else gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, data);
            column(i);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'texture upload between draws, column ' + i);
    }

    // =====================================================================
    // 8-bit indices: each draw widens its own indices.
    // =====================================================================
    {
        reset();
        gl.useProgram(colorProg);
        const aColor = gl.getAttribLocation(colorProg, 'aColor');
        // Two quads' worth of vertices: left half red, right half blue.
        const verts = new Float32Array([
            -1, -1, 0, -1, -1, 1, 0, 1,     // left quad
             0, -1, 1, -1,  0, 1, 1, 1]);   // right quad
        const cols = new Float32Array(32);
        for (let k = 0; k < 4; ++k) cols.set([1, 0, 0, 1], k * 4);
        for (let k = 4; k < 8; ++k) cols.set([0, 0, 1, 1], k * 4);
        const vb = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, vb);
        gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);
        const cb = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, cb);
        gl.bufferData(gl.ARRAY_BUFFER, cols, gl.STATIC_DRAW);
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, vb);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ARRAY_BUFFER, cb);
        gl.enableVertexAttribArray(aColor);
        gl.vertexAttribPointer(aColor, 4, gl.FLOAT, false, 0, 0);
        const left = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, left);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint8Array([0, 1, 2, 2, 1, 3]), gl.STATIC_DRAW);
        gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_BYTE, 0);
        const right = gl.createBuffer();
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, right);
        gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint8Array([4, 5, 6, 6, 5, 7]), gl.STATIC_DRAW);
        gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_BYTE, 0);
        assertPx(px(16, 32), 255, 0, 0, '8-bit indices, first draw');
        assertPx(px(48, 32), 0, 0, 255, '8-bit indices, second draw');
        gl.bindVertexArray(null);
    }

    // =====================================================================
    // More linked programs than the old fixed descriptor pool held (128),
    // each drawing with its own constant colour.
    // =====================================================================
    {
        reset();
        const programs = [];
        for (let i = 0; i < 160; ++i) {
            const c = palette[i % 8];
            programs.push(makeProgram(kVs, '#version 300 es\nprecision highp float;\nout vec4 frag;\n' +
                'void main(){ frag = vec4(' + (c[0] / 255).toFixed(4) + ',' + (c[1] / 255).toFixed(4) + ',' +
                (c[2] / 255).toFixed(4) + ', 1.0 + 0.0 * float(' + i + ')); }'));
        }
        assert(gl.getError() === gl.NO_ERROR, 'no error linking 160 programs');
        gl.bindVertexArray(quadVao);
        for (let i = 0; i < 8; ++i) {
            column(i);
            gl.useProgram(programs[152 + i]);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[(152 + i) % 8], 'program #' + (152 + i));
        for (const p of programs) gl.deleteProgram(p);
    }

    // =====================================================================
    // Work recorded in one frame and read after the frame ends still lands.
    // =====================================================================
    {
        reset();
        const prog = makeProgram(kVs, '#version 300 es\nprecision highp float;\nuniform vec4 uColor;\n' +
            'out vec4 frag;\nvoid main(){ frag = uColor; }');
        gl.useProgram(prog);
        gl.bindVertexArray(quadVao);
        const uColor = gl.getUniformLocation(prog, 'uColor');
        for (let i = 0; i < 8; ++i) {
            const c = palette[i];
            column(i);
            gl.uniform4f(uColor, c[0] / 255, c[1] / 255, c[2] / 255, 1);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        }
        advanceTime(16);
        for (let i = 0; i < 8; ++i) assertPx(colPx(i), ...palette[i], 'across a frame boundary, column ' + i);
    }

    // =====================================================================
    // Fences are real: unsignaled work becomes signaled once waited for.
    // =====================================================================
    {
        gl.clear(gl.COLOR_BUFFER_BIT);
        const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
        const r = gl.clientWaitSync(sync, 0, 1e8);
        assert(r === gl.ALREADY_SIGNALED || r === gl.CONDITION_SATISFIED,
               'clientWaitSync with a timeout completes (got 0x' + r.toString(16) + ')');
        assert(gl.getSyncParameter(sync, gl.SYNC_STATUS) === gl.SIGNALED, 'fence signaled after the wait');
        gl.deleteSync(sync);
    }

    assert(gl.getError() === gl.NO_ERROR, 'no GL error at the end');
}
