// Texture units: 32 a stage and 64 a program where the device's descriptor
// limits allow (what Chromium reports, and what pixi.js sizes its batch
// shader by), and a sampler array of MAX_TEXTURE_IMAGE_UNITS elements links,
// binds and samples each unit's own texture.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '8');
canvas.setAttribute('height', '8');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2', { antialias: false });
if (!gl) {
    missingGpuContext('webgl2');
} else {
    const units = gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS);
    const vertexUnits = gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    const combined = gl.getParameter(gl.MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    assert(units >= 16 && units <= 32, 'MAX_TEXTURE_IMAGE_UNITS within [16, 32]: ' + units);
    assert(vertexUnits === units, 'MAX_VERTEX_TEXTURE_IMAGE_UNITS is the fragment stage\'s: ' + vertexUnits);
    assert(combined >= units && combined <= 64, 'MAX_COMBINED_TEXTURE_IMAGE_UNITS within [units, 64]: ' + combined);

    // Every unit below the combined limit is a unit; the limit is not.
    gl.activeTexture(gl.TEXTURE0 + combined - 1);
    assert(gl.getError() === gl.NO_ERROR, 'activeTexture(TEXTURE0 + combined - 1) is valid');
    gl.activeTexture(gl.TEXTURE0 + combined);
    assert(gl.getError() === gl.INVALID_ENUM, 'activeTexture(TEXTURE0 + combined) is INVALID_ENUM');
    const tail = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0 + combined - 1);
    gl.bindTexture(gl.TEXTURE_2D, tail);
    assert(gl.getParameter(gl.TEXTURE_BINDING_2D) === tail, 'the last unit holds a binding');
    gl.bindTexture(gl.TEXTURE_2D, null);
    gl.activeTexture(gl.TEXTURE0);

    // One 1x1 texture per unit, unit i holding the value i.
    for (let i = 0; i < units; ++i) {
        const t = gl.createTexture();
        gl.activeTexture(gl.TEXTURE0 + i);
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([i * 7, 255 - i, i, 255]));
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    }

    const vs = `#version 300 es
        void main() {
            vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 4.0 - 1.0;
            gl_Position = vec4(p, 0.0, 1.0);
        }`;
    // pixi's batch shader: a sampler array indexed in a switch.
    let cases = '';
    for (let i = 0; i < units; ++i) cases += `case ${i}: c = texture(uTextures[${i}], vec2(0.5)); break;\n`;
    const fs = `#version 300 es
        precision highp float;
        uniform sampler2D uTextures[${units}];
        uniform int which;
        out vec4 c;
        void main() { switch (which) { ${cases} default: c = vec4(0.0); } }`;
    const p = gl.createProgram();
    for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        gl.attachShader(p, s);
    }
    gl.linkProgram(p);
    assert(gl.getProgramParameter(p, gl.LINK_STATUS), 'a ' + units + '-sampler array links: ' + gl.getProgramInfoLog(p));
    const info = gl.getActiveUniform(p, 0).name === 'uTextures[0]' ? gl.getActiveUniform(p, 0) : gl.getActiveUniform(p, 1);
    assert(info.name === 'uTextures[0]' && info.size === units, 'uTextures[0] has size ' + units + ': ' + info.size);
    gl.useProgram(p);
    gl.uniform1iv(gl.getUniformLocation(p, 'uTextures[0]'), Int32Array.from({ length: units }, (_, i) => i));
    assert(gl.getError() === gl.NO_ERROR, 'the units upload');

    const px = new Uint8Array(4);
    for (const i of [0, 1, units >> 1, units - 2, units - 1]) {
        gl.uniform1i(gl.getUniformLocation(p, 'which'), i);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.readPixels(4, 4, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        assert(px[0] === i * 7 && px[1] === 255 - i && px[2] === i, 'unit ' + i + ' samples its own texture: ' + px);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error');
}
