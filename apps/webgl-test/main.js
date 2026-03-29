// Minimal WebGL2 test: colored triangle

// Test Phase 4 stubs
console.log('devicePixelRatio: ' + window.devicePixelRatio);
console.log('navigator.userAgent: ' + navigator.userAgent);
console.log('innerWidth x innerHeight: ' + innerWidth + 'x' + innerHeight);

var img = new Image();
console.log('Image created, complete=' + img.complete);
img.onload = function() { console.log('Image onload fired: ' + img.width + 'x' + img.height); };
img.src = 'test.png';

var canvas = document.querySelector('#c');
console.log('canvas.width=' + canvas.width + ' canvas.height=' + canvas.height);
console.log('canvas.clientWidth=' + canvas.clientWidth + ' canvas.clientHeight=' + canvas.clientHeight);

var gl = canvas.getContext('webgl2');

if (!gl) {
    console.log('ERROR: Failed to get WebGL2 context');
} else {
    console.log('WebGL2 context created successfully!');
    console.log('  VERSION: ' + gl.getParameter(gl.VERSION));
    console.log('  RENDERER: ' + gl.getParameter(gl.RENDERER));
    console.log('  MAX_TEXTURE_SIZE: ' + gl.getParameter(gl.MAX_TEXTURE_SIZE));

    // Vertex shader
    var vsSource = `#version 300 es
    layout(location = 0) in vec2 aPosition;
    layout(location = 1) in vec3 aColor;
    out vec3 vColor;
    void main() {
        gl_Position = vec4(aPosition, 0.0, 1.0);
        vColor = aColor;
    }`;

    // Fragment shader
    var fsSource = `#version 300 es
    precision highp float;
    in vec3 vColor;
    out vec4 fragColor;
    void main() {
        fragColor = vec4(vColor, 1.0);
    }`;

    // Compile shaders
    var vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, vsSource);
    gl.compileShader(vs);
    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
        console.log('VS error: ' + gl.getShaderInfoLog(vs));
    }

    var fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, fsSource);
    gl.compileShader(fs);
    if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        console.log('FS error: ' + gl.getShaderInfoLog(fs));
    }

    // Link program
    var prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
        console.log('Link error: ' + gl.getProgramInfoLog(prog));
    }

    // Triangle vertices: position (x,y) + color (r,g,b)
    var vertices = new Float32Array([
         0.0,  0.5,  1.0, 0.0, 0.0,
        -0.5, -0.5,  0.0, 1.0, 0.0,
         0.5, -0.5,  0.0, 0.0, 1.0,
    ]);

    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    var vbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 20, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 20, 8);

    gl.bindVertexArray(null);

    console.log('Setup complete, starting render loop');

    function render() {
        gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
        gl.clearColor(0.1, 0.1, 0.15, 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT);

        gl.useProgram(prog);
        gl.bindVertexArray(vao);
        gl.drawArrays(gl.TRIANGLES, 0, 3);

        requestAnimationFrame(render);
    }
    requestAnimationFrame(render);
}
