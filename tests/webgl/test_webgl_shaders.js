// WebGL2 conformance subset — shaders & programs: compile/link failure
// reporting, getActiveUniform/getActiveAttrib metadata, every uniform setter
// shape (scalar / vecN / matN / arrays / plain JS arrays) verified by
// rendering, uniform blocks (UBO), getUniformBlockIndex/INVALID_INDEX.
// Exercises src/js/webgl2_bindings_shaders.cpp.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '64');
canvas.setAttribute('height', '64');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    console.log('no webgl2; skipping');
} else {
    function compile(type, src) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        return s;
    }
    function makeProgram(vsSrc, fsSrc) {
        const vs = compile(gl.VERTEX_SHADER, vsSrc);
        if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS))
            throw new Error('vs: ' + gl.getShaderInfoLog(vs));
        const fs = compile(gl.FRAGMENT_SHADER, fsSrc);
        if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS))
            throw new Error('fs: ' + gl.getShaderInfoLog(fs));
        const p = gl.createProgram();
        gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS))
            throw new Error('link: ' + gl.getProgramInfoLog(p));
        gl.deleteShader(vs); gl.deleteShader(fs);
        return p;
    }
    function near(a, b, tol) { return Math.abs(a - b) <= (tol || 3); }
    function assertPx(p, r, g, b, a, msg) {
        assert(near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a),
               msg + ' got [' + Array.from(p).join(',') + '] want [' + [r, g, b, a].join(',') + ']');
    }

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);

    const vsPass = '#version 300 es\nin vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }';

    function drawAndRead() {
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
        const b = new Uint8Array(4);
        gl.readPixels(32, 32, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return b;
    }

    // =====================================================================
    // Compile errors are reported, not thrown
    // =====================================================================
    const bad = compile(gl.VERTEX_SHADER, 'this is not GLSL at all;');
    assert(gl.getShaderParameter(bad, gl.COMPILE_STATUS) === false, 'bad shader compile status false');
    assert(String(gl.getShaderInfoLog(bad)).length > 0, 'compile error log non-empty');
    assert(gl.getShaderParameter(bad, gl.SHADER_TYPE) === gl.VERTEX_SHADER, 'SHADER_TYPE query');
    gl.deleteShader(bad);

    // Link error: varying type mismatch, both statically used
    const vsMismatch = compile(gl.VERTEX_SHADER,
        '#version 300 es\nin vec2 aPos;\nout vec2 vX;\n' +
        'void main(){ vX = aPos; gl_Position = vec4(aPos, 0.0, 1.0); }');
    const fsMismatch = compile(gl.FRAGMENT_SHADER,
        '#version 300 es\nprecision highp float;\nin vec3 vX;\nout vec4 frag;\n' +
        'void main(){ frag = vec4(vX, 1.0); }');
    assert(gl.getShaderParameter(vsMismatch, gl.COMPILE_STATUS) === true, 'mismatch vs compiles');
    assert(gl.getShaderParameter(fsMismatch, gl.COMPILE_STATUS) === true, 'mismatch fs compiles');
    const badProg = gl.createProgram();
    gl.attachShader(badProg, vsMismatch);
    gl.attachShader(badProg, fsMismatch);
    gl.linkProgram(badProg);
    assert(gl.getProgramParameter(badProg, gl.LINK_STATUS) === false, 'varying mismatch link fails');
    assert(String(gl.getProgramInfoLog(badProg)).length > 0, 'link error log non-empty');
    assert(gl.getProgramParameter(badProg, gl.ATTACHED_SHADERS) === 2, 'ATTACHED_SHADERS 2');
    gl.detachShader(badProg, fsMismatch);
    assert(gl.getProgramParameter(badProg, gl.ATTACHED_SHADERS) === 1, 'ATTACHED_SHADERS after detach');
    gl.deleteShader(vsMismatch);
    gl.deleteShader(fsMismatch);
    gl.deleteProgram(badProg);
    assert(gl.getError() === gl.NO_ERROR, 'no GL error from compile/link failures');

    // =====================================================================
    // getActiveUniform / getActiveAttrib metadata
    // =====================================================================
    const progMeta = makeProgram(
        '#version 300 es\nin vec2 aP;\nin vec4 aQ;\n' +
        'void main(){ gl_Position = vec4(aP, 0.0, 1.0) + aQ * 0.0001; }',
        '#version 300 es\nprecision highp float;\n' +
        'uniform float uF;\nuniform vec3 uV[2];\nuniform sampler2D uS;\nout vec4 frag;\n' +
        'void main(){ frag = vec4(uF) + vec4(uV[0] + uV[1], 1.0) + texture(uS, vec2(0.5)); }');
    const nAttribs = gl.getProgramParameter(progMeta, gl.ACTIVE_ATTRIBUTES);
    assert(nAttribs === 2, 'ACTIVE_ATTRIBUTES 2 (got ' + nAttribs + ')');
    const attribNames = {};
    for (let i = 0; i < nAttribs; i++) {
        const info = gl.getActiveAttrib(progMeta, i);
        attribNames[info.name] = info;
    }
    assert(attribNames['aP'] && attribNames['aP'].type === gl.FLOAT_VEC2 && attribNames['aP'].size === 1,
           'aP metadata');
    assert(attribNames['aQ'] && attribNames['aQ'].type === gl.FLOAT_VEC4, 'aQ metadata');

    const nUniforms = gl.getProgramParameter(progMeta, gl.ACTIVE_UNIFORMS);
    assert(nUniforms === 3, 'ACTIVE_UNIFORMS 3 (got ' + nUniforms + ')');
    const uniNames = {};
    for (let i = 0; i < nUniforms; i++) {
        const info = gl.getActiveUniform(progMeta, i);
        uniNames[info.name] = info;
    }
    assert(uniNames['uF'] && uniNames['uF'].type === gl.FLOAT && uniNames['uF'].size === 1, 'uF metadata');
    assert(uniNames['uV[0]'] && uniNames['uV[0]'].type === gl.FLOAT_VEC3 && uniNames['uV[0]'].size === 2,
           'uV[0] array metadata');
    assert(uniNames['uS'] && uniNames['uS'].type === gl.SAMPLER_2D, 'uS sampler metadata');
    gl.deleteProgram(progMeta);

    // =====================================================================
    // Float uniform setters — scalar, fv typed array, fv plain JS array
    // =====================================================================
    const progF = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\n' +
        'uniform float uF;\nuniform vec2 uV2;\nuniform vec3 uV3;\nuniform vec4 uV4;\nout vec4 frag;\n' +
        'void main(){ frag = vec4(uF, uV2.y, uV3.z, uV4.w); }');
    gl.useProgram(progF);
    const locF = gl.getUniformLocation(progF, 'uF');
    const locV2 = gl.getUniformLocation(progF, 'uV2');
    const locV3 = gl.getUniformLocation(progF, 'uV3');
    const locV4 = gl.getUniformLocation(progF, 'uV4');
    assert(locF !== null && locV2 !== null && locV3 !== null && locV4 !== null, 'uniform locations');
    assert(gl.getUniformLocation(progF, 'uMissing') === null, 'missing uniform is null');

    gl.uniform1f(locF, 0.25);
    gl.uniform2f(locV2, 0, 0.5);
    gl.uniform3f(locV3, 0, 0, 0.75);
    gl.uniform4f(locV4, 0, 0, 0, 1.0);
    assertPx(drawAndRead(), 64, 128, 191, 255, 'scalar float setters');

    gl.uniform1fv(locF, new Float32Array([0.5]));
    gl.uniform2fv(locV2, new Float32Array([0, 0.25]));
    gl.uniform3fv(locV3, new Float32Array([0, 0, 1.0]));
    gl.uniform4fv(locV4, new Float32Array([0, 0, 0, 0.75]));
    assertPx(drawAndRead(), 128, 64, 255, 191, 'typed-array fv setters');

    gl.uniform1fv(locF, [1.0]);
    gl.uniform2fv(locV2, [0, 1.0]);
    gl.uniform3fv(locV3, [0, 0, 0.25]);
    gl.uniform4fv(locV4, [0, 0, 0, 0.5]);
    assertPx(drawAndRead(), 255, 255, 64, 128, 'plain-JS-array fv setters');

    // Null location is a silent no-op
    gl.uniform1f(null, 123);
    assert(gl.getError() === gl.NO_ERROR, 'uniform on null location no-ops');
    gl.deleteProgram(progF);

    // =====================================================================
    // Int uniforms + float array uniform (uniform1fv over float[2])
    // =====================================================================
    const progI = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\n' +
        'uniform ivec4 uI;\nuniform float uArr[2];\nout vec4 frag;\n' +
        'void main(){ frag = vec4(float(uI.x)/255.0, float(uI.y)/255.0, uArr[0], uArr[1]); }');
    gl.useProgram(progI);
    const locI = gl.getUniformLocation(progI, 'uI');
    const locArr = gl.getUniformLocation(progI, 'uArr');
    gl.uniform4i(locI, 32, 200, 0, 0);
    gl.uniform1fv(locArr, new Float32Array([0.5, 1.0]));
    assertPx(drawAndRead(), 32, 200, 128, 255, 'ivec4 + float[] uniforms');
    gl.uniform4iv(locI, new Int32Array([200, 32, 0, 0]));
    gl.uniform1fv(locArr, [1.0, 0.5]);
    assertPx(drawAndRead(), 200, 32, 255, 128, 'iv typed array + plain array float[]');
    gl.uniform4iv(locI, [64, 128, 0, 0]);
    assertPx(drawAndRead(), 64, 128, 255, 128, 'uniform4iv plain JS array');
    gl.deleteProgram(progI);

    // =====================================================================
    // Matrix uniforms — square and non-square
    // =====================================================================
    const progM = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\n' +
        'uniform mat2 uM2;\nuniform mat3 uM3;\nuniform mat4 uM4;\nout vec4 frag;\n' +
        'void main(){ frag = vec4(uM2[0][0], uM3[1][1], uM4[2][2], uM4[3][3]); }');
    gl.useProgram(progM);
    gl.uniformMatrix2fv(gl.getUniformLocation(progM, 'uM2'), false,
                        new Float32Array([0.25, 0, 0, 0]));
    gl.uniformMatrix3fv(gl.getUniformLocation(progM, 'uM3'), false,
                        new Float32Array([0, 0, 0, 0, 0.5, 0, 0, 0, 0]));
    gl.uniformMatrix4fv(gl.getUniformLocation(progM, 'uM4'), false,
                        new Float32Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.75, 0, 0, 0, 0, 1]));
    assertPx(drawAndRead(), 64, 128, 191, 255, 'mat2/mat3/mat4 uniforms');
    gl.deleteProgram(progM);

    const progM23 = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\n' +
        'uniform mat2x3 uM23;\nuniform mat3x2 uM32;\nout vec4 frag;\n' +
        'void main(){ vec3 c = uM23 * vec2(1.0, 0.0);\n' +
        '  frag = vec4(c.rg, uM32[2][1], 1.0); }');
    gl.useProgram(progM23);
    // Column-major: first column of mat2x3 is elements 0..2
    gl.uniformMatrix2x3fv(gl.getUniformLocation(progM23, 'uM23'), false,
                          new Float32Array([0.5, 0.25, 0, 0, 0, 0]));
    // mat3x2 column 2 is elements 4..5; [2][1] is element 5
    gl.uniformMatrix3x2fv(gl.getUniformLocation(progM23, 'uM32'), false,
                          new Float32Array([0, 0, 0, 0, 0, 0.75]));
    assertPx(drawAndRead(), 128, 64, 191, 255, 'mat2x3 + mat3x2 uniforms');
    gl.deleteProgram(progM23);

    // =====================================================================
    // Uniform blocks — UBO round-trip drives draw output
    // =====================================================================
    const progU = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\n' +
        'layout(std140) uniform Colors { vec4 uCol; };\nout vec4 frag;\n' +
        'void main(){ frag = uCol; }');
    gl.useProgram(progU);
    const blockIdx = gl.getUniformBlockIndex(progU, 'Colors');
    assert(blockIdx !== gl.INVALID_INDEX, 'getUniformBlockIndex finds block');
    assert(gl.getUniformBlockIndex(progU, 'Nope') === gl.INVALID_INDEX,
           'missing block -> INVALID_INDEX');
    assert(gl.getProgramParameter(progU, gl.ACTIVE_UNIFORM_BLOCKS) === 1, 'ACTIVE_UNIFORM_BLOCKS 1');

    const ubo = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
    gl.bufferData(gl.UNIFORM_BUFFER, new Float32Array([0, 1, 0.5, 1]), gl.DYNAMIC_DRAW);
    gl.uniformBlockBinding(progU, blockIdx, 2);
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 2, ubo);
    assertPx(drawAndRead(), 0, 255, 128, 255, 'UBO values drive draw output');

    // Update the buffer, redraw — new values flow through
    gl.bindBuffer(gl.UNIFORM_BUFFER, ubo);
    gl.bufferSubData(gl.UNIFORM_BUFFER, 0, new Float32Array([1, 0, 0.25, 1]));
    assertPx(drawAndRead(), 255, 0, 64, 255, 'UBO update flows to next draw');
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 2, null);
    gl.deleteBuffer(ubo);
    gl.deleteProgram(progU);

    // =====================================================================
    // getFragDataLocation on a single-output program
    // =====================================================================
    const progOut = makeProgram(vsPass,
        '#version 300 es\nprecision highp float;\nout vec4 fragColor;\n' +
        'void main(){ fragColor = vec4(1.0); }');
    assert(gl.getFragDataLocation(progOut, 'fragColor') === 0, 'implicit frag output location 0');
    gl.deleteProgram(progOut);

    // bindAttribLocation before link is honoured
    const vsB = compile(gl.VERTEX_SHADER, vsPass.replace('aPos', 'aBound').replace('aPos', 'aBound'));
    const fsB = compile(gl.FRAGMENT_SHADER,
        '#version 300 es\nprecision highp float;\nout vec4 frag;\nvoid main(){ frag = vec4(1.0); }');
    const progB = gl.createProgram();
    gl.attachShader(progB, vsB);
    gl.attachShader(progB, fsB);
    gl.bindAttribLocation(progB, 3, 'aBound');
    gl.linkProgram(progB);
    assert(gl.getProgramParameter(progB, gl.LINK_STATUS) === true, 'bound-attrib program links');
    assert(gl.getAttribLocation(progB, 'aBound') === 3, 'bindAttribLocation honoured');
    gl.deleteShader(vsB);
    gl.deleteShader(fsB);
    gl.deleteProgram(progB);

    gl.deleteBuffer(quadBuf);
    gl.deleteVertexArray(vao);
    assert(gl.getError() === gl.NO_ERROR, 'no error at end');

    console.log('webgl shader tests passed');
}

document.body.removeChild(canvas);
