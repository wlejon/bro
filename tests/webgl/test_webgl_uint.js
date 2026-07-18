// WebGL2 conformance subset — unsigned-integer uniforms and constant integer
// vertex attributes: uniform1ui..4ui, uniform*uiv (typed arrays AND plain JS
// arrays per the sequence<GLuint> overloads), vertexAttribI4i/ui/iv/uiv, all
// verified numerically via pixels through uint shader plumbing.
// Exercises src/js/webgl2_bindings_shaders.cpp, _buffers.cpp +
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

    const prog = makeProgram(
        '#version 300 es\n' +
        'layout(location = 0) in vec2 aPos;\n' +
        'layout(location = 1) in uvec4 aInt;\n' +
        'flat out uvec4 vInt;\n' +
        'void main(){ vInt = aInt; gl_Position = vec4(aPos, 0.0, 1.0); }',
        '#version 300 es\nprecision highp float;\nprecision highp int;\n' +
        'flat in uvec4 vInt;\n' +
        'uniform uint uA;\nuniform uvec2 uB;\nuniform uvec3 uC;\nuniform uvec4 uD;\n' +
        'uniform uint uMode;\nout vec4 frag;\n' +
        'void main(){\n' +
        '  if (uMode == 0u) frag = vec4(float(uA), float(uB.x), float(uB.y), 255.0) / 255.0;\n' +
        '  else if (uMode == 1u) frag = vec4(vec3(uC), 255.0) / 255.0;\n' +
        '  else if (uMode == 2u) frag = vec4(uD) / 255.0;\n' +
        '  else frag = vec4(vInt) / 255.0;\n' +
        '}');
    gl.useProgram(prog);
    const uA = gl.getUniformLocation(prog, 'uA');
    const uB = gl.getUniformLocation(prog, 'uB');
    const uC = gl.getUniformLocation(prog, 'uC');
    const uD = gl.getUniformLocation(prog, 'uD');
    const uMode = gl.getUniformLocation(prog, 'uMode');

    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, 64, 64);
    function draw() { gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4); }

    // =====================================================================
    // Scalar setters: uniform1ui + uniform2ui
    // =====================================================================
    gl.uniform1ui(uMode, 0);
    gl.uniform1ui(uA, 255);
    gl.uniform2ui(uB, 128, 64);
    draw();
    assertPx(px(32, 32), 255, 128, 64, 'uniform1ui/2ui');
    assert(gl.getError() === gl.NO_ERROR, 'no error after scalar ui setters');

    // =====================================================================
    // Vector setters: uniform3uiv (plain JS array) + uniform4uiv (typed)
    // =====================================================================
    gl.uniform1ui(uMode, 1);
    gl.uniform3uiv(uC, [32, 192, 250]); // sequence<GLuint>
    draw();
    assertPx(px(32, 32), 32, 192, 250, 'uniform3uiv plain array');

    gl.uniform1ui(uMode, 2);
    gl.uniform4uiv(uD, new Uint32Array([10, 20, 30, 255]));
    draw();
    assertPx(px(32, 32), 10, 20, 30, 'uniform4uiv typed array');

    // uniform3ui / uniform4ui scalar forms overwrite the same slots.
    gl.uniform1ui(uMode, 1);
    gl.uniform3ui(uC, 200, 100, 50);
    draw();
    assertPx(px(32, 32), 200, 100, 50, 'uniform3ui');
    gl.uniform1ui(uMode, 2);
    gl.uniform4ui(uD, 90, 80, 70, 255);
    draw();
    assertPx(px(32, 32), 90, 80, 70, 'uniform4ui');
    assert(gl.getError() === gl.NO_ERROR, 'no error after vector ui setters');

    // =====================================================================
    // Constant integer vertex attribute (array disabled at location 1)
    // =====================================================================
    gl.uniform1ui(uMode, 3);
    gl.vertexAttribI4ui(1, 0, 255, 0, 255);
    draw();
    assertPx(px(32, 32), 0, 255, 0, 'vertexAttribI4ui constant attribute');

    gl.vertexAttribI4uiv(1, new Uint32Array([255, 0, 255, 255]));
    draw();
    assertPx(px(32, 32), 255, 0, 255, 'vertexAttribI4uiv');

    gl.vertexAttribI4iv(1, [255, 255, 0, 255]); // plain array accepted
    draw();
    assertPx(px(32, 32), 255, 255, 0, 'vertexAttribI4iv plain array');

    gl.vertexAttribI4i(1, 64, 64, 255, 255);
    draw();
    assertPx(px(32, 32), 64, 64, 255, 'vertexAttribI4i');
    assert(gl.getError() === gl.NO_ERROR, 'no error after constant int attribs');

    console.log('webgl uint tests passed');
}

document.body.removeChild(canvas);
