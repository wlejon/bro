// WebGL2 conformance subset — program/uniform-block introspection against a
// std140 UBO whose layout is spec-fixed (offsets asserted exactly):
// getUniformIndices, getActiveUniforms (TYPE/SIZE/BLOCK_INDEX/OFFSET/
// MATRIX_STRIDE/IS_ROW_MAJOR), getActiveUniformBlockParameter,
// getActiveUniformBlockName, uniformBlockBinding round-trip. Plus the full
// is* object-predicate sweep (lifecycle + cross-type + null).
// Exercises src/js/webgl2_bindings_shaders.cpp and friends +
// src/webgl/webgl2_context.cpp.

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

    // std140: vec4 @ 0, float @ 16, mat4 @ 32 (16-aligned), total 96.
    const prog = makeProgram(
        '#version 300 es\n' +
        'in vec2 aPos;\n' +
        'layout(std140) uniform Settings { vec4 uColorA; float uScale; mat4 uMat; };\n' +
        'out vec4 vCol;\n' +
        'void main(){ vCol = uColorA * uScale * uMat[0][0]; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nin vec4 vCol;\nout vec4 frag;\n' +
        'void main(){ frag = vCol; }');
    gl.useProgram(prog);

    // =====================================================================
    // Block-level introspection
    // =====================================================================
    const blockIdx = gl.getUniformBlockIndex(prog, 'Settings');
    assert(blockIdx !== gl.INVALID_INDEX, 'getUniformBlockIndex finds the block');
    assert(gl.getActiveUniformBlockName(prog, blockIdx) === 'Settings',
           'getActiveUniformBlockName');
    assert(gl.getActiveUniformBlockParameter(prog, blockIdx, gl.UNIFORM_BLOCK_DATA_SIZE) >= 96,
           'UNIFORM_BLOCK_DATA_SIZE covers the std140 layout');
    assert(gl.getActiveUniformBlockParameter(prog, blockIdx, gl.UNIFORM_BLOCK_ACTIVE_UNIFORMS) === 3,
           'UNIFORM_BLOCK_ACTIVE_UNIFORMS');
    const memberIdxs = gl.getActiveUniformBlockParameter(prog, blockIdx,
                                                         gl.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES);
    assert(Array.isArray(memberIdxs) && memberIdxs.length === 3,
           'UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES length');
    assert(gl.getActiveUniformBlockParameter(prog, blockIdx,
               gl.UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER) === true,
           'REFERENCED_BY_VERTEX_SHADER true');
    assert(gl.getActiveUniformBlockParameter(prog, blockIdx,
               gl.UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER) === false,
           'REFERENCED_BY_FRAGMENT_SHADER false');

    gl.uniformBlockBinding(prog, blockIdx, 2);
    assert(gl.getActiveUniformBlockParameter(prog, blockIdx, gl.UNIFORM_BLOCK_BINDING) === 2,
           'uniformBlockBinding round-trip');

    // =====================================================================
    // Member-level introspection by name
    // =====================================================================
    const idxs = gl.getUniformIndices(prog, ['uColorA', 'uScale', 'uMat', 'doesNotExist']);
    assert(idxs.length === 4, 'getUniformIndices length');
    assert(idxs[0] !== gl.INVALID_INDEX && idxs[1] !== gl.INVALID_INDEX &&
           idxs[2] !== gl.INVALID_INDEX, 'members resolve');
    assert(idxs[3] === gl.INVALID_INDEX, 'unknown name -> INVALID_INDEX');

    const three = [idxs[0], idxs[1], idxs[2]];
    const offsets = gl.getActiveUniforms(prog, three, gl.UNIFORM_OFFSET);
    assert(offsets[0] === 0 && offsets[1] === 16 && offsets[2] === 32,
           'std140 UNIFORM_OFFSET [0,16,32] got [' + offsets.join(',') + ']');
    const types = gl.getActiveUniforms(prog, three, gl.UNIFORM_TYPE);
    assert(types[0] === gl.FLOAT_VEC4 && types[1] === gl.FLOAT && types[2] === gl.FLOAT_MAT4,
           'UNIFORM_TYPE per member');
    const sizes = gl.getActiveUniforms(prog, three, gl.UNIFORM_SIZE);
    assert(sizes[0] === 1 && sizes[1] === 1 && sizes[2] === 1, 'UNIFORM_SIZE all 1');
    const blockOf = gl.getActiveUniforms(prog, three, gl.UNIFORM_BLOCK_INDEX);
    assert(blockOf.every(function(b) { return b === blockIdx; }), 'UNIFORM_BLOCK_INDEX per member');
    const mstride = gl.getActiveUniforms(prog, three, gl.UNIFORM_MATRIX_STRIDE);
    assert(mstride[2] === 16, 'mat4 UNIFORM_MATRIX_STRIDE 16');
    assert(mstride[0] === 0 && mstride[1] === 0, 'non-matrix UNIFORM_MATRIX_STRIDE 0');
    const rowMajor = gl.getActiveUniforms(prog, three, gl.UNIFORM_IS_ROW_MAJOR);
    assert(rowMajor.every(function(r) { return r === false; }), 'UNIFORM_IS_ROW_MAJOR booleans');
    // getUniformIndices accepts a typed-array of indices too.
    const offsets2 = gl.getActiveUniforms(prog, new Uint32Array(three), gl.UNIFORM_OFFSET);
    assert(offsets2[0] === 0 && offsets2[2] === 32, 'getActiveUniforms typed-array indices');
    assert(gl.getError() === gl.NO_ERROR, 'no error after introspection');

    // =====================================================================
    // is* predicate sweep — gen-style objects: false before first bind,
    // true bound, false deleted (id-reuse-proof via the context valid sets)
    // =====================================================================
    const buf = gl.createBuffer();
    assert(gl.isBuffer(buf) === false, 'isBuffer false before bind');
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    assert(gl.isBuffer(buf) === true, 'isBuffer true after bind');
    gl.deleteBuffer(buf);
    assert(gl.isBuffer(buf) === false, 'isBuffer false after delete');

    const tex = gl.createTexture();
    assert(gl.isTexture(tex) === false, 'isTexture false before bind');
    gl.bindTexture(gl.TEXTURE_2D, tex);
    assert(gl.isTexture(tex) === true, 'isTexture true after bind');
    gl.deleteTexture(tex);
    assert(gl.isTexture(tex) === false, 'isTexture false after delete');

    const fbo = gl.createFramebuffer();
    assert(gl.isFramebuffer(fbo) === false, 'isFramebuffer false before bind');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    assert(gl.isFramebuffer(fbo) === true, 'isFramebuffer true after bind');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fbo);
    assert(gl.isFramebuffer(fbo) === false, 'isFramebuffer false after delete');

    const rbo = gl.createRenderbuffer();
    assert(gl.isRenderbuffer(rbo) === false, 'isRenderbuffer false before bind');
    gl.bindRenderbuffer(gl.RENDERBUFFER, rbo);
    assert(gl.isRenderbuffer(rbo) === true, 'isRenderbuffer true after bind');
    gl.deleteRenderbuffer(rbo);
    assert(gl.isRenderbuffer(rbo) === false, 'isRenderbuffer false after delete');

    const vao = gl.createVertexArray();
    assert(gl.isVertexArray(vao) === false, 'isVertexArray false before bind');
    gl.bindVertexArray(vao);
    assert(gl.isVertexArray(vao) === true, 'isVertexArray true after bind');
    gl.bindVertexArray(null);
    gl.deleteVertexArray(vao);
    assert(gl.isVertexArray(vao) === false, 'isVertexArray false after delete');

    // Create-style objects: true straight after create.
    const p2 = gl.createProgram();
    assert(gl.isProgram(p2) === true, 'isProgram true after create');
    gl.deleteProgram(p2); // not in use -> deleted immediately
    assert(gl.isProgram(p2) === false, 'isProgram false after delete');
    const sh = gl.createShader(gl.VERTEX_SHADER);
    assert(gl.isShader(sh) === true, 'isShader true after create');
    gl.deleteShader(sh); // unattached -> deleted immediately
    assert(gl.isShader(sh) === false, 'isShader false after delete');

    // Cross-type and null inputs are false, never an error.
    const b2 = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, b2);
    const t2 = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t2);
    assert(gl.isBuffer(t2) === false, 'isBuffer(texture) false');
    assert(gl.isTexture(b2) === false, 'isTexture(buffer) false');
    assert(gl.isBuffer(null) === false, 'isBuffer(null) false');
    assert(gl.isProgram(null) === false, 'isProgram(null) false');
    gl.deleteBuffer(b2);
    gl.deleteTexture(t2);
    assert(gl.getError() === gl.NO_ERROR, 'no error after is* sweep');

    console.log('webgl introspection tests passed');
}

document.body.removeChild(canvas);
