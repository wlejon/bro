// The GLSL front end (glslang + reflection): shapes a line-oriented parser
// gets wrong, checked by rendering. Single-line sources, GLSL ES 1.00
// (attribute / varying / gl_FragColor / texture2D), struct and array
// uniforms addressed by name, transposed and non-square matrices, uniform
// blocks read through bindBufferRange at an offset, attribute location
// placement (layout, bindAttribLocation, automatic), and the resource
// limits getParameter reports being the ones a link enforces.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '8');
canvas.setAttribute('height', '8');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function build(vsSrc, fsSrc, beforeLink) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vsSrc], [gl.FRAGMENT_SHADER, fsSrc]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            assert(gl.getShaderParameter(s, gl.COMPILE_STATUS), 'compile: ' + gl.getShaderInfoLog(s) + '\n' + src);
            gl.attachShader(p, s);
        }
        if (beforeLink) beforeLink(p);
        gl.linkProgram(p);
        return p;
    }
    function program(vsSrc, fsSrc, beforeLink) {
        const p = build(vsSrc, fsSrc, beforeLink);
        assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'link: ' + gl.getProgramInfoLog(p));
        gl.useProgram(p);
        return p;
    }
    // A full-viewport quad through attribute location `loc`.
    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    function drawQuad(loc) {
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
        gl.enableVertexAttribArray(loc);
        gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        gl.deleteVertexArray(vao);
    }
    function assertColor(want, msg) {
        const px = new Uint8Array(4);
        gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        const w = want.map(v => Math.round(v * 255));
        assert(Array.from(px).every((v, i) => Math.abs(v - w[i]) <= 2),
               msg + ': got [' + Array.from(px) + '] want [' + w + ']');
        assert(gl.getError() === gl.NO_ERROR, msg + ': no GL error');
    }
    gl.viewport(0, 0, 8, 8);

    // --- single-line sources ------------------------------------------------
    let p = program('attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }',
                    'precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }');
    gl.uniform4f(gl.getUniformLocation(p, 'c'), 0.25, 0.5, 0.75, 1.0);
    drawQuad(gl.getAttribLocation(p, 'p'));
    assertColor([0.25, 0.5, 0.75, 1.0], 'single-line GLSL ES 1.00');

    p = program('#version 300 es\nin vec2 p; out vec2 v; void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }',
                '#version 300 es\nprecision highp float; in vec2 v; out vec4 o; uniform float k; void main() { o = vec4(k, 0.0, 1.0 - k, 1.0); }');
    gl.uniform1f(gl.getUniformLocation(p, 'k'), 0.4);
    drawQuad(gl.getAttribLocation(p, 'p'));
    assertColor([0.4, 0.0, 0.6, 1.0], 'single-line GLSL ES 3.00 after the #version line');

    // --- GLSL ES 1.00: varyings, texture2D, uniform arrays -------------------
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0, 128, 255, 255]));
    p = program(
        'attribute vec2 aPos;\nvarying vec2 vUv;\nvoid main() {\n  vUv = aPos * 0.5 + 0.5;\n  gl_Position = vec4(aPos, 0.0, 1.0);\n}\n',
        'precision mediump float;\nvarying vec2 vUv;\nuniform sampler2D uTex;\nuniform float uW[3];\n' +
        'void main() {\n  vec4 t = texture2D(uTex, vUv);\n  gl_FragColor = vec4(uW[0] + uW[2], t.g, t.b * uW[1], 1.0);\n}\n');
    gl.uniform1i(gl.getUniformLocation(p, 'uTex'), 0);
    gl.uniform1fv(gl.getUniformLocation(p, 'uW'), [0.1, 0.5, 0.3]);
    drawQuad(gl.getAttribLocation(p, 'aPos'));
    assertColor([0.4, 128 / 255, 0.5, 1.0], 'GLSL ES 1.00 varyings + texture2D + uniform array');
    gl.uniform1f(gl.getUniformLocation(p, 'uW[2]'), 0.0);
    drawQuad(gl.getAttribLocation(p, 'aPos'));
    assertColor([0.1, 128 / 255, 0.5, 1.0], 'uniform array element by name');

    // --- struct uniforms, arrays of structs ---------------------------------
    p = program('#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }',
                '#version 300 es\nprecision highp float;\nstruct Light { vec3 color; float scale; };\n' +
                'uniform Light uLights[2];\nuniform Light uOne;\nout vec4 o;\n' +
                'void main(){ o = vec4(uLights[0].color * uLights[0].scale + uLights[1].color * uLights[1].scale + uOne.color * uOne.scale, 1.0); }');
    gl.uniform3f(gl.getUniformLocation(p, 'uLights[0].color'), 1, 0, 0);
    gl.uniform1f(gl.getUniformLocation(p, 'uLights[0].scale'), 0.5);
    gl.uniform3f(gl.getUniformLocation(p, 'uLights[1].color'), 0, 1, 0);
    gl.uniform1f(gl.getUniformLocation(p, 'uLights[1].scale'), 0.25);
    gl.uniform3f(gl.getUniformLocation(p, 'uOne.color'), 0, 0, 1);
    gl.uniform1f(gl.getUniformLocation(p, 'uOne.scale'), 0.75);
    drawQuad(gl.getAttribLocation(p, 'p'));
    assertColor([0.5, 0.25, 0.75, 1.0], 'struct and array-of-struct uniforms');

    // --- matrices: transpose, non-square -------------------------------------
    p = program('#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }',
                '#version 300 es\nprecision highp float;\nuniform mat2 uM2;\nuniform mat2x3 uM23;\nuniform mat3x4 uM34;\nout vec4 o;\n' +
                'void main(){ vec2 a = uM2 * vec2(1.0, 0.0); vec3 b = uM23 * vec2(0.0, 1.0); vec4 c = uM34 * vec3(0.0, 0.0, 1.0);\n' +
                '  o = vec4(a.y, b.z, c.w, 1.0); }');
    // mat2 column 0 is (m[0], m[1]); transposed, row 0 is: a.y = m[2].
    gl.uniformMatrix2fv(gl.getUniformLocation(p, 'uM2'), true, [0.0, 0.0, 0.6, 0.0]);
    // mat2x3: two columns of vec3; column 1's z is element 5.
    gl.uniformMatrix2x3fv(gl.getUniformLocation(p, 'uM23'), false, [0, 0, 0, 0, 0, 0.3]);
    // mat3x4: three columns of vec4; column 2's w is element 11.
    gl.uniformMatrix3x4fv(gl.getUniformLocation(p, 'uM34'), false, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.9]);
    drawQuad(gl.getAttribLocation(p, 'p'));
    assertColor([0.6, 0.3, 0.9, 1.0], 'transposed mat2, mat2x3, mat3x4');

    // --- uniform blocks through bindBufferRange at an offset ------------------
    const align = gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    assert(align > 0 && (align & (align - 1)) === 0, 'UNIFORM_BUFFER_OFFSET_ALIGNMENT is a power of two: ' + align);
    p = program('#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }',
                '#version 300 es\nprecision highp float;\nlayout(std140) uniform Material { vec4 tint; float gain; };\n' +
                'layout(std140) uniform Extra { vec4 add; };\nout vec4 o;\nvoid main(){ o = tint * gain + add; }');
    const ubo = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
    // Blocks placed `stride` apart: a multiple of the alignment past 32 bytes.
    const stride = Math.ceil(32 / align) * align;
    const floats = new Float32Array((stride * 3) / 4);
    floats.set([9, 9, 9, 9, 9], 0);                               // decoy at offset 0
    floats.set([0.2, 0.4, 0.6, 1.0, 0.5], stride / 4);            // Material
    floats.set([0.1, 0.1, 0.1, 0.0], (stride * 2) / 4);           // Extra
    gl.bufferData(gl.UNIFORM_BUFFER, floats, gl.STATIC_DRAW);
    gl.uniformBlockBinding(p, gl.getUniformBlockIndex(p, 'Material'), 3);
    gl.uniformBlockBinding(p, gl.getUniformBlockIndex(p, 'Extra'), 5);
    gl.bindBufferRange(gl.UNIFORM_BUFFER, 3, ubo, stride, 32);
    gl.bindBufferRange(gl.UNIFORM_BUFFER, 5, ubo, stride * 2, 16);
    assert(gl.getIndexedParameter(gl.UNIFORM_BUFFER_START, 3) === stride, 'UNIFORM_BUFFER_START');
    assert(gl.getIndexedParameter(gl.UNIFORM_BUFFER_SIZE, 5) === 16, 'UNIFORM_BUFFER_SIZE');
    drawQuad(gl.getAttribLocation(p, 'p'));
    assertColor([0.2, 0.3, 0.4, 0.5], 'uniform blocks read at their bindBufferRange offsets');
    gl.bindBufferRange(gl.UNIFORM_BUFFER, 3, ubo, stride, 4);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    assert(gl.getError() === gl.INVALID_OPERATION, 'a range smaller than the block is INVALID_OPERATION at draw');
    gl.bindBufferRange(gl.UNIFORM_BUFFER, 3, ubo, 1, 32);
    assert(align === 1 || gl.getError() === gl.INVALID_VALUE, 'a misaligned offset is INVALID_VALUE');

    // --- attribute locations ----------------------------------------------------
    p = program('#version 300 es\nlayout(location = 5) in vec2 aFixed;\nin vec4 aBound;\nin vec4 aAuto;\n' +
                'out vec4 v;\nvoid main(){ v = aBound + aAuto; gl_Position = vec4(aFixed, 0.0, 1.0); }',
                '#version 300 es\nprecision highp float;\nin vec4 v;\nout vec4 o;\nvoid main(){ o = v; }',
                prog => gl.bindAttribLocation(prog, 2, 'aBound'));
    assert(gl.getAttribLocation(p, 'aFixed') === 5, 'layout(location) honoured');
    assert(gl.getAttribLocation(p, 'aBound') === 2, 'bindAttribLocation honoured');
    const autoLoc = gl.getAttribLocation(p, 'aAuto');
    assert(autoLoc >= 0 && autoLoc !== 5 && autoLoc !== 2, 'automatic location avoids the taken ones: ' + autoLoc);
    gl.vertexAttrib4f(2, 0.5, 0.0, 0.0, 0.5);
    gl.vertexAttrib4f(autoLoc, 0.0, 0.25, 0.0, 0.5);
    drawQuad(5);
    assertColor([0.5, 0.25, 0.0, 1.0], 'constant attributes reach their locations');
    const clash = build('#version 300 es\nin vec2 a;\nin vec2 b;\nvoid main(){ gl_Position = vec4(a + b, 0.0, 1.0); }',
                        '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){ o = vec4(1.0); }',
                        prog => { gl.bindAttribLocation(prog, 1, 'a'); gl.bindAttribLocation(prog, 1, 'b'); });
    assert(!gl.getProgramParameter(clash, gl.LINK_STATUS), 'two attributes bound to one location fail to link');

    // --- limits getParameter reports are the ones a link enforces ------------
    const maxVec = gl.getParameter(gl.MAX_FRAGMENT_UNIFORM_VECTORS);
    assert(maxVec >= 224, 'MAX_FRAGMENT_UNIFORM_VECTORS meets WebGL 2 minimum: ' + maxVec);
    const fits = build('#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }',
                       '#version 300 es\nprecision highp float;\nuniform vec4 u[' + (maxVec - 1) + '];\nuniform int i;\nout vec4 o;\nvoid main(){ o = u[i]; }');
    assert(gl.getProgramParameter(fits, gl.LINK_STATUS), 'MAX_FRAGMENT_UNIFORM_VECTORS vectors (an array and an int) link: ' +
           gl.getProgramInfoLog(fits));
    const tooMany = build('#version 300 es\nin vec2 p;\nvoid main(){ gl_Position = vec4(p, 0.0, 1.0); }',
                          '#version 300 es\nprecision highp float;\nuniform vec4 u[' + maxVec + '];\nuniform int i;\nout vec4 o;\nvoid main(){ o = u[i]; }');
    assert(!gl.getProgramParameter(tooMany, gl.LINK_STATUS), 'one vector past the limit fails to link');
    // Samplers: each stage up to its own limit, the program up to the combined
    // one (WebGL 2 minimums 16 / 16 / 32: both stages full at once must link).
    const maxTex = gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS);
    const maxVsTex = gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    const maxCombined = gl.getParameter(gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    assert(maxTex >= 16 && maxVsTex >= 16 && maxCombined >= 32,
           'texture unit limits meet WebGL 2 minimums: ' + [maxTex, maxVsTex, maxCombined]);
    const sampleAll = (n) => Array.from({ length: n }, (_, k) => 'texture(t[' + k + '], vec2(0.5))').join(' + ');
    const vsSampling = (n) => '#version 300 es\nin vec2 p;\nuniform sampler2D t[' + n + '];\nout vec4 c;\n' +
                              'void main(){ c = ' + sampleAll(n) + '; gl_Position = vec4(p, 0.0, 1.0); }';
    const fsSampling = (n) => '#version 300 es\nprecision highp float;\nin vec4 c;\nuniform sampler2D s[' + n + '];\nout vec4 o;\n' +
                              'void main(){ o = c + ' + sampleAll(n).replace(/t\[/g, 's[') + '; }';
    const bothFull = build(vsSampling(maxVsTex), fsSampling(Math.min(maxTex, maxCombined - maxVsTex)));
    assert(gl.getProgramParameter(bothFull, gl.LINK_STATUS), 'both stages at their sampler limits link: ' +
           gl.getProgramInfoLog(bothFull));
    const fsOver = build(vsSampling(1), fsSampling(maxTex + 1));
    assert(!gl.getProgramParameter(fsOver, gl.LINK_STATUS), 'one sampler past MAX_TEXTURE_IMAGE_UNITS fails to link');
    assert(gl.getParameter(gl.MAX_VERTEX_ATTRIBS) >= 16, 'MAX_VERTEX_ATTRIBS >= 16');
    assert(gl.getParameter(gl.MAX_TEXTURE_SIZE) >= 2048, 'MAX_TEXTURE_SIZE >= 2048');
    assert(gl.getParameter(gl.MAX_DRAW_BUFFERS) >= 4, 'MAX_DRAW_BUFFERS >= 4');
    assert(gl.getParameter(gl.MAX_UNIFORM_BUFFER_BINDINGS) >= 24, 'MAX_UNIFORM_BUFFER_BINDINGS >= 24');
    assert(gl.getParameter(gl.MAX_ELEMENT_INDEX) >= 16777215, 'MAX_ELEMENT_INDEX >= 2^24 - 1');
    const lineRange = gl.getParameter(gl.ALIASED_LINE_WIDTH_RANGE);
    assert(lineRange[0] <= 1 && lineRange[1] >= 1, 'one-pixel lines are in ALIASED_LINE_WIDTH_RANGE: ' + lineRange);
    const maxRb = gl.getParameter(gl.MAX_RENDERBUFFER_SIZE);
    const rb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, maxRb + 1, 1);
    assert(gl.getError() === gl.INVALID_VALUE, 'a renderbuffer past MAX_RENDERBUFFER_SIZE is INVALID_VALUE');
    assert(gl.getError() === gl.NO_ERROR, 'end of test');
}
