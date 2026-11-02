// Active uniforms are the ones the linked program reads, as a GL driver
// reports them: a uniform declared but never used (three.js declares
// modelMatrix, viewMatrix, cameraPosition, ... in every program's prefix) is
// not in ACTIVE_UNIFORMS, getActiveUniform never names it, and its
// getUniformLocation is null. Members of a uniform block are all active (a
// std140 block's layout is the declaration's), and a struct array's members
// are active one by one.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '8');
canvas.setAttribute('height', '8');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function program(vs, fs) {
        const p = gl.createProgram();
        for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
            const s = gl.createShader(type);
            gl.shaderSource(s, src);
            gl.compileShader(s);
            if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
            gl.attachShader(p, s);
        }
        gl.linkProgram(p);
        if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
        return p;
    }
    function activeNames(p) {
        const n = gl.getProgramParameter(p, gl.ACTIVE_UNIFORMS);
        const names = [];
        for (let i = 0; i < n; ++i) names.push(gl.getActiveUniform(p, i).name);
        return names.sort();
    }
    function same(got, want, msg) {
        assert(got.length === want.length && got.every((v, i) => v === want[i]),
               msg + ': got [' + got + '] want [' + want + ']');
    }

    const vs = `#version 300 es
        uniform mat4 modelMatrix;          // declared, never read
        uniform mat4 modelViewMatrix;
        uniform mat4 projectionMatrix;
        uniform mat4 viewMatrix;           // declared, never read
        uniform vec3 cameraPosition;       // read only by a function nothing calls
        struct Light { vec3 direction; vec3 color; float intensity; };
        uniform Light lights[2];
        in vec3 position;
        out vec3 vColor;
        vec3 unusedHelper() { return cameraPosition; }  // never called
        void main() {
            vColor = lights[0].direction + lights[1].color;
            gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }`;
    const fs = `#version 300 es
        precision highp float;
        uniform vec3 diffuse;
        uniform float opacity;             // declared, never read
        uniform sampler2D unusedMap;       // declared, never sampled
        uniform sampler2D map;
        layout(std140) uniform Material { vec4 tint; vec4 unusedMember; };
        in vec3 vColor;
        out vec4 color;
        void main() { color = vec4(diffuse + vColor, 1.0) * texture(map, vec2(0.5)) * tint; }`;
    const p = program(vs, fs);

    same(activeNames(p),
         ['diffuse', 'lights[0].direction', 'lights[1].color', 'map', 'modelViewMatrix', 'projectionMatrix',
          'tint', 'unusedMember'].sort(),
         'ACTIVE_UNIFORMS are the read ones, plus every member of the block');

    for (const name of ['modelMatrix', 'viewMatrix', 'cameraPosition', 'opacity', 'unusedMap',
                        'lights[0].color', 'lights[0].intensity', 'lights[1].direction'])
        assert(gl.getUniformLocation(p, name) === null, 'getUniformLocation(' + name + ') is null: inactive');
    for (const name of ['modelViewMatrix', 'projectionMatrix', 'diffuse', 'map', 'lights[0].direction',
                        'lights[1].color'])
        assert(gl.getUniformLocation(p, name) !== null, 'getUniformLocation(' + name + ') is a location');

    // Uploading through a null location is a no-op, not an error.
    gl.useProgram(p);
    gl.uniformMatrix4fv(gl.getUniformLocation(p, 'modelMatrix'), false, new Float32Array(16));
    gl.uniform1f(gl.getUniformLocation(p, 'opacity'), 1);
    assert(gl.getError() === gl.NO_ERROR, 'a null location uploads nothing, with no error');

    // The active ones still upload and read back.
    const diffuse = gl.getUniformLocation(p, 'diffuse');
    gl.uniform3f(diffuse, 0.25, 0.5, 0.75);
    const back = gl.getUniform(p, diffuse);
    assert(back[0] === 0.25 && back[1] === 0.5 && back[2] === 0.75, 'an active uniform reads back: ' + back);
    const dir = gl.getUniformLocation(p, 'lights[0].direction');
    gl.uniform3f(dir, 1, 2, 3);
    assert(gl.getUniform(p, dir)[2] === 3, 'a struct-array member reads back');

    // The block keeps both members, at their std140 offsets.
    const block = gl.getUniformBlockIndex(p, 'Material');
    const idx = gl.getActiveUniformBlockParameter(p, block, gl.UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES);
    assert(idx.length === 2, 'the block lists both of its members: ' + idx.length);
    const offsets = gl.getActiveUniforms(p, Array.from(idx), gl.UNIFORM_OFFSET).slice().sort((a, b) => a - b);
    assert(offsets[0] === 0 && offsets[1] === 16, 'std140 offsets 0 and 16: ' + offsets);

    // A program that reads every uniform it declares loses none of them.
    const all = program(`#version 300 es
        uniform mat4 m; uniform vec2 offsets[3];
        void main() { gl_Position = m * vec4(offsets[0] + offsets[2], 0.0, 1.0); }`,
        `#version 300 es
        precision highp float; uniform float k; out vec4 c; void main() { c = vec4(k); }`);
    same(activeNames(all), ['k', 'm', 'offsets[0]'], 'every read uniform is active, an array as one');
    for (let i = 0; i < 3; ++i) {
        const info = gl.getActiveUniform(all, i);
        if (info.name === 'offsets[0]') assert(info.size === 3, 'offsets has size 3: ' + info.size);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
