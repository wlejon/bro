// Three.js WebGL2 test: noise-displaced terrain with custom shaders

console.log('devicePixelRatio: ' + window.devicePixelRatio);
console.log('navigator.userAgent: ' + navigator.userAgent);
console.log('innerWidth x innerHeight: ' + innerWidth + 'x' + innerHeight);

var canvas = document.querySelector('#c');

var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: true });
renderer.setSize(canvas.width, canvas.height);
console.log('Three.js renderer created');

var scene = new THREE.Scene();
scene.background = new THREE.Color(0.1, 0.1, 0.15);

var camera = new THREE.PerspectiveCamera(60, canvas.width / canvas.height, 0.1, 100);
camera.position.set(0, 1.5, 2.0);
camera.lookAt(0, 0, 0);

// Load shaders via brokit fs
var fs = globalThis.__brokit_fs;
var vertexShader = fs.readFileSync('shaders/custom.vert', 'utf-8');
var fragmentShader = fs.readFileSync('shaders/custom.frag', 'utf-8');
console.log('Shaders loaded');

// Create terrain plane
var geometry = new THREE.PlaneGeometry(4, 4, 128, 128);
geometry.rotateX(-Math.PI / 2); // lay flat (XZ plane, Y is up)
var count = geometry.attributes.position.count;

// Create custom attributes
var customPos = new Float32Array(count * 3);
var customColor = new Float32Array(count * 4);

for (var i = 0; i < count; i++) {
    // Small random offset per vertex (subtle detail)
    customPos[i * 3    ] = (Math.random() - 0.5) * 0.05;
    customPos[i * 3 + 1] = (Math.random() - 0.5) * 0.05;
    customPos[i * 3 + 2] = (Math.random() - 0.5) * 0.05;

    // Color gradient across the terrain (X → red, Z → blue)
    var x = geometry.attributes.position.getX(i);
    var z = geometry.attributes.position.getZ(i);
    customColor[i * 4    ] = (x + 2) / 4; // red: left to right
    customColor[i * 4 + 1] = 0.5;         // green: constant
    customColor[i * 4 + 2] = (z + 2) / 4; // blue: front to back
    customColor[i * 4 + 3] = 1.0;
}

geometry.setAttribute('aCustomPos', new THREE.BufferAttribute(customPos, 3));
geometry.setAttribute('aCustomColor', new THREE.BufferAttribute(customColor, 4));

// ShaderMaterial with custom uniforms
var material = new THREE.ShaderMaterial({
    vertexShader: vertexShader,
    fragmentShader: fragmentShader,
    uniforms: {
        uTime: { value: 0.0 },
        uScale: { value: 0.4 }
    },
    side: THREE.DoubleSide
});

var mesh = new THREE.Mesh(geometry, material);
scene.add(mesh);

console.log('Setup complete, starting render loop');

var clock = new THREE.Clock();

function render() {
    var elapsed = clock.getElapsedTime();
    material.uniforms.uTime.value = elapsed * 0.5;

    renderer.render(scene, camera);
    requestAnimationFrame(render);
}
requestAnimationFrame(render);
