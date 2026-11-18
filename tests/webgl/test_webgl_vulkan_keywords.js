// Words that are identifiers in GLSL ES but keywords of the Vulkan GLSL bro
// compiles WebGL shaders as (`sampler`, `buffer`, `shared`, and in ES 3.00
// `texture2D` and the other texture types) compile, and the program reports
// them by the names the source uses; so do the GLSL 450 built-ins ES 1.00
// lacks (`texture`, `round`, `sample`) in ES 1.00 source. three.js's transmission shader has a
// function parameter named `sampler` and failed to compile.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '4');
canvas.setAttribute('height', '4');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const vs = `#version 300 es
    in vec2 shared;
    out vec4 buffer;
    void main() { gl_Position = vec4(shared, 0, 1); buffer = vec4(0.25); }`;
    const fs = `#version 300 es
    precision highp float;
    // a comment naming sampler is left alone
    #define texture2D texture
    uniform sampler2D tex;
    uniform float precise;
    in vec4 buffer;
    out vec4 sampler;
    vec4 fetch(sampler2D sampler, vec2 uv) { return texture2D(sampler, uv); }
    void main() { vec4 textureCube = fetch(tex, vec2(0.5)); sampler = vec4(textureCube.r, buffer.g, precise, 1.0); }`;
    const p = gl.createProgram();
    for (const [t, s] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
        const sh = gl.createShader(t);
        gl.shaderSource(sh, s);
        gl.compileShader(sh);
        assert(gl.getShaderParameter(sh, gl.COMPILE_STATUS), 'compiles: ' + gl.getShaderInfoLog(sh));
        gl.attachShader(p, sh);
    }
    gl.linkProgram(p);
    assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'links: ' + gl.getProgramInfoLog(p));
    assert(gl.getAttribLocation(p, 'shared') === 0, 'the attribute is named shared');
    assert(gl.getActiveAttrib(p, 0).name === 'shared', 'getActiveAttrib names it shared');
    assert(gl.getFragDataLocation(p, 'sampler') === 0, 'the output is named sampler');
    const names = [];
    for (let i = 0; i < gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS); ++i) names.push(gl.getActiveUniform(p, i).name);
    assert(names.sort().join() === 'precise,tex', 'uniform names, got ' + names.join());
    gl.useProgram(p);
    gl.uniform1f(gl.getUniformLocation(p, 'precise'), 0.5);

    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 0, 0, 255]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.uniform1i(gl.getUniformLocation(p, 'tex'), 0);

    const b = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    const px = new Uint8Array(4);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    const got = Array.from(px).join(',');
    assert(got === '255,64,128,255', 'draws through the renamed names, got ' + got);

    // ES 1.00: the GLSL 450 functions it lacks are the program's own names
    // (Phaser's sprite shader: `vec4 texture = texture2D(uMainSampler, ...)`).
    const vs1 = 'attribute vec2 round; void main() { gl_Position = vec4(round, 0.0, 1.0); }';
    const fs1 = 'precision mediump float; uniform sampler2D uMainSampler; uniform float sample;\n' +
        'float inverse(float x) { return 1.0 - x; }\n' +
        'void main() { vec4 texture = texture2D(uMainSampler, vec2(0.5)); gl_FragColor = vec4(texture.r, inverse(sample), 0.0, 1.0); }';
    const p1 = gl.createProgram();
    for (const [t, s1] of [[gl.VERTEX_SHADER, vs1], [gl.FRAGMENT_SHADER, fs1]]) {
        const sh = gl.createShader(t);
        gl.shaderSource(sh, s1);
        gl.compileShader(sh);
        assert(gl.getShaderParameter(sh, gl.COMPILE_STATUS), 'ES 1.00 compiles: ' + gl.getShaderInfoLog(sh));
        gl.attachShader(p1, sh);
    }
    gl.linkProgram(p1);
    assert(gl.getProgramParameter(p1, gl.LINK_STATUS), 'ES 1.00 links: ' + gl.getProgramInfoLog(p1));
    assert(gl.getAttribLocation(p1, 'round') >= 0 && gl.getUniformLocation(p1, 'sample') !== null, 'by their own names');
    gl.useProgram(p1);
    gl.uniform1f(gl.getUniformLocation(p1, 'sample'), 0.75);
    gl.uniform1i(gl.getUniformLocation(p1, 'uMainSampler'), 0);
    const loc = gl.getAttribLocation(p1, 'round');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
    assert(Array.from(px).join(',') === '255,64,0,255', 'ES 1.00 draws, got ' + Array.from(px).join(','));
}
