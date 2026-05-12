// Test WebGL2 context — exercises src/webgl/webgl2_context.cpp,
// src/js/webgl2_bindings*.cpp (buffers, shaders, state, textures,
// framebuffers).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    // No GPU — skip
    console.log('no webgl2; skipping');
} else {
    // =====================================================================
    // Context info
    // =====================================================================
    assert(gl.drawingBufferWidth === 128, 'drawingBufferWidth');
    assert(gl.drawingBufferHeight === 128, 'drawingBufferHeight');

    const version = gl.getParameter(gl.VERSION);
    assert(typeof version === 'string', 'VERSION is string');

    const renderer = gl.getParameter(gl.RENDERER);
    assert(typeof renderer === 'string', 'RENDERER is string');

    // =====================================================================
    // State setters
    // =====================================================================
    gl.viewport(0, 0, 128, 128);
    gl.scissor(0, 0, 64, 64);
    gl.enable(gl.SCISSOR_TEST);
    gl.disable(gl.SCISSOR_TEST);
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthFunc(gl.LEQUAL);
    gl.cullFace(gl.BACK);
    gl.frontFace(gl.CCW);

    gl.clearColor(0.1, 0.2, 0.3, 1.0);
    gl.clearDepth(1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // =====================================================================
    // Buffers
    // =====================================================================
    const vbo = gl.createBuffer();
    assert(vbo !== null, 'createBuffer returns object');
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    const verts = new Float32Array([
        -0.5, -0.5,
         0.5, -0.5,
         0.0,  0.5,
    ]);
    gl.bufferData(gl.ARRAY_BUFFER, verts, gl.STATIC_DRAW);

    // bufferSubData
    const upd = new Float32Array([1, 2]);
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, upd);

    const ibo = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2]), gl.STATIC_DRAW);

    // =====================================================================
    // Shaders + program
    // =====================================================================
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, '#version 300 es\nin vec2 aPos;\nvoid main() { gl_Position = vec4(aPos, 0.0, 1.0); }');
    gl.compileShader(vs);
    const vsOk = gl.getShaderParameter(vs, gl.COMPILE_STATUS);
    if (!vsOk) console.log('vs log:', gl.getShaderInfoLog(vs));
    assert(vsOk === true, 'vertex shader compiled');

    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs,
        '#version 300 es\nprecision highp float;\nout vec4 frag;\nvoid main() { frag = vec4(1.0, 0.5, 0.25, 1.0); }');
    gl.compileShader(fs);
    const fsOk = gl.getShaderParameter(fs, gl.COMPILE_STATUS);
    if (!fsOk) console.log('fs log:', gl.getShaderInfoLog(fs));
    assert(fsOk === true, 'fragment shader compiled');

    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS) === true, 'program linked');

    gl.useProgram(prog);
    const aPos = gl.getAttribLocation(prog, 'aPos');
    assert(aPos >= 0, 'aPos location found');

    // VAO
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.enableVertexAttribArray(aPos);
    gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 0, 0);

    // Draw call
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // =====================================================================
    // Texture
    // =====================================================================
    const tex = gl.createTexture();
    assert(tex !== null, 'createTexture');
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    const pixels = new Uint8Array([
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 0, 255,
    ]);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 2, 2, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels);

    // texSubImage2D
    const sub = new Uint8Array([0, 0, 0, 255]);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, sub);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);

    // =====================================================================
    // Framebuffer
    // =====================================================================
    const fbo = gl.createFramebuffer();
    assert(fbo !== null, 'createFramebuffer');
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);

    const rbo = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rbo);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, 64, 64);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, rbo);

    const fboTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, fboTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 64, 64, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, fboTex, 0);

    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    assert(status === gl.FRAMEBUFFER_COMPLETE, 'fbo complete, got ' + status);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null); // unbind

    // =====================================================================
    // Uniforms
    // =====================================================================
    const vs2 = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs2,
        '#version 300 es\nuniform float uVal;\nuniform vec3 uColor;\nuniform mat4 uMat;\nvoid main() { gl_Position = uMat * vec4(uVal * uColor, 1.0); }');
    gl.compileShader(vs2);

    const fs2 = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs2,
        '#version 300 es\nprecision highp float;\nout vec4 f;\nvoid main() { f = vec4(1.0); }');
    gl.compileShader(fs2);

    const prog2 = gl.createProgram();
    gl.attachShader(prog2, vs2);
    gl.attachShader(prog2, fs2);
    gl.linkProgram(prog2);
    assert(gl.getProgramParameter(prog2, gl.LINK_STATUS), 'prog2 linked');
    gl.useProgram(prog2);

    const uVal = gl.getUniformLocation(prog2, 'uVal');
    gl.uniform1f(uVal, 2.5);

    const uColor = gl.getUniformLocation(prog2, 'uColor');
    gl.uniform3f(uColor, 1, 0.5, 0.25);
    gl.uniform3fv(uColor, new Float32Array([1, 0.5, 0.25]));

    const uMat = gl.getUniformLocation(prog2, 'uMat');
    gl.uniformMatrix4fv(uMat, false, new Float32Array([
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
    ]));

    // =====================================================================
    // Cleanup
    // =====================================================================
    gl.deleteBuffer(vbo);
    gl.deleteBuffer(ibo);
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    gl.deleteShader(vs2);
    gl.deleteShader(fs2);
    gl.deleteProgram(prog);
    gl.deleteProgram(prog2);
    gl.deleteTexture(tex);
    gl.deleteTexture(fboTex);
    gl.deleteFramebuffer(fbo);
    gl.deleteRenderbuffer(rbo);
    gl.deleteVertexArray(vao);
}

document.body.removeChild(canvas);
