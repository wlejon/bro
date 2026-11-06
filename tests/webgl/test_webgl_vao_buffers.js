// Vertex array objects and the buffers they name, draw after draw: two VAOs
// drawn alternately each read their own buffers; a VAO re-pointed at another
// buffer, or its element array rebound, reads the new one at the next draw;
// a buffer storage re-specified by bufferData is read at its new size; and a
// draw whose attribute or element buffer was deleted is INVALID_OPERATION,
// also after the same VAO drew successfully before (the context resolves a
// VAO's buffers once and must notice each of these).

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '16');
canvas.setAttribute('height', '16');
document.body.appendChild(canvas);
flush();

const gl = canvas.getContext('webgl2');
if (!gl) {
    missingGpuContext('webgl2');
} else {
    function compile(type, src) {
        const s = gl.createShader(type);
        gl.shaderSource(s, src);
        gl.compileShader(s);
        if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
        return s;
    }
    const prog = gl.createProgram();
    gl.attachShader(prog, compile(gl.VERTEX_SHADER,
        '#version 300 es\nlayout(location = 0) in vec2 aPos;\nlayout(location = 1) in vec4 aCol;\n' +
        'out vec4 vCol;\nvoid main(){ vCol = aCol; gl_Position = vec4(aPos, 0.0, 1.0); }'));
    gl.attachShader(prog, compile(gl.FRAGMENT_SHADER,
        '#version 300 es\nprecision highp float;\nin vec4 vCol;\nout vec4 o;\nvoid main(){ o = vCol; }'));
    gl.linkProgram(prog);
    assert(gl.getProgramParameter(prog, gl.LINK_STATUS), 'link');
    gl.useProgram(prog);
    gl.viewport(0, 0, 16, 16);

    function buffer(target, data) {
        const b = gl.createBuffer();
        gl.bindBuffer(target, b);
        gl.bufferData(target, data, gl.STATIC_DRAW);
        return b;
    }
    const quad = new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]);
    const colors = (r, g, b) => new Float32Array([r, g, b, 1, r, g, b, 1, r, g, b, 1, r, g, b, 1]);
    function makeVao(posBuf, colBuf, ibo) {
        const vao = gl.createVertexArray();
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, posBuf);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ARRAY_BUFFER, colBuf);
        gl.enableVertexAttribArray(1);
        gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
        gl.bindVertexArray(null);
        return vao;
    }
    function draw(vao) {
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.bindVertexArray(vao);
        gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
        const p = new Uint8Array(4);
        gl.readPixels(8, 8, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, p);
        return Array.from(p).join(',');
    }

    const pos = buffer(gl.ARRAY_BUFFER, quad);
    const red = buffer(gl.ARRAY_BUFFER, colors(1, 0, 0));
    const green = buffer(gl.ARRAY_BUFFER, colors(0, 1, 0));
    const blue = buffer(gl.ARRAY_BUFFER, colors(0, 0, 1));
    const ibo = buffer(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2, 2, 1, 3]));
    const vaoA = makeVao(pos, red, ibo);
    const vaoB = makeVao(pos, green, ibo);

    for (let i = 0; i < 3; i++) {
        assert(draw(vaoA) === '255,0,0,255', 'VAO A draws red, round ' + i);
        assert(draw(vaoB) === '0,255,0,255', 'VAO B draws green, round ' + i);
    }
    assert(gl.getError() === gl.NO_ERROR, 'no error drawing the two VAOs');

    // Re-pointed at another buffer.
    gl.bindVertexArray(vaoA);
    gl.bindBuffer(gl.ARRAY_BUFFER, blue);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 0, 0);
    assert(draw(vaoA) === '0,0,255,255', 'VAO A re-pointed at the blue buffer');

    // The element array rebound: indices that draw only the lower-left
    // triangle, so the centre pixel of the upper right is left clear.
    gl.bindVertexArray(null);
    const iboLow = buffer(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array([0, 1, 2, 0, 1, 2]));
    gl.bindVertexArray(vaoB);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, iboLow);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawElements(gl.TRIANGLES, 6, gl.UNSIGNED_SHORT, 0);
    const corner = new Uint8Array(4);
    gl.readPixels(14, 14, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, corner);
    assert(Array.from(corner).join(',') === '0,0,0,0', 'VAO B reads its rebound element array');
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ibo);
    assert(draw(vaoB) === '0,255,0,255', 'VAO B with its first element array again');

    // Storage re-specified smaller than the draw reads: an error, then fine
    // again once it is large enough.
    gl.bindBuffer(gl.ARRAY_BUFFER, green);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(4), gl.STATIC_DRAW);
    draw(vaoB);
    assert(gl.getError() === gl.INVALID_OPERATION, 'reading past a shrunk buffer is INVALID_OPERATION');
    gl.bufferData(gl.ARRAY_BUFFER, colors(1, 1, 0), gl.STATIC_DRAW);
    assert(draw(vaoB) === '255,255,0,255', 'the re-specified buffer is read');
    assert(gl.getError() === gl.NO_ERROR, 'no error after re-specifying');

    // Deleted buffers, after the VAO drew with them.
    assert(draw(vaoA) === '0,0,255,255', 'VAO A before the delete');
    gl.bindVertexArray(null);
    gl.deleteBuffer(blue);
    draw(vaoA);
    assert(gl.getError() === gl.INVALID_OPERATION, 'an attribute buffer deleted: INVALID_OPERATION');
    assert(draw(vaoB) === '255,255,0,255', 'VAO B unaffected by the delete');
    gl.bindVertexArray(null);
    gl.deleteBuffer(ibo);
    draw(vaoB);
    assert(gl.getError() === gl.INVALID_OPERATION, 'the element buffer deleted: INVALID_OPERATION');
}
