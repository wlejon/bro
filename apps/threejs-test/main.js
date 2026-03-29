// three.js test - spinning textured cube with lighting
var canvas = document.getElementById('c');
console.log('THREE version:', THREE.REVISION);

var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setSize(1024, 768, false);

var scene = new THREE.Scene();
scene.background = new THREE.Color(0x334455);

var camera = new THREE.PerspectiveCamera(75, 1024 / 768, 0.1, 1000);
camera.position.z = 4;

// Load checkerboard texture
var textureLoader = new THREE.TextureLoader();
var texture = textureLoader.load('checker.png');

// Textured cube (center) - unlit to test texture
var cube = new THREE.Mesh(
    new THREE.BoxGeometry(1.2, 1.2, 1.2),
    new THREE.MeshBasicMaterial({ map: texture })
);
scene.add(cube);

// Red cube (right)
var cube2 = new THREE.Mesh(
    new THREE.BoxGeometry(0.6, 0.6, 0.6),
    new THREE.MeshBasicMaterial({ color: 0xff4444 })
);
cube2.position.x = 2.5;
scene.add(cube2);

// Green cube (left)
var cube3 = new THREE.Mesh(
    new THREE.BoxGeometry(0.6, 0.6, 0.6),
    new THREE.MeshBasicMaterial({ color: 0x44ff44 })
);
cube3.position.x = -2.5;
scene.add(cube3);

console.log('Scene ready');

function animate() {
    requestAnimationFrame(animate);
    cube.rotation.x += 0.01;
    cube.rotation.y += 0.02;
    cube2.rotation.y += 0.03;
    cube3.rotation.x += 0.02;
    renderer.render(scene, camera);
}
animate();
