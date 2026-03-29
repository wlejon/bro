// three.js test - spinning cube with Phong lighting
var canvas = document.getElementById('c');
console.log('THREE version:', THREE.REVISION);

var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setSize(1024, 768, false);

var scene = new THREE.Scene();
scene.background = new THREE.Color(0x334455);

var camera = new THREE.PerspectiveCamera(75, 1024 / 768, 0.1, 1000);
camera.position.z = 3;

// Geometry + material
var geometry = new THREE.BoxGeometry(1, 1, 1);
var material = new THREE.MeshPhongMaterial({ color: 0x4488ff, shininess: 60 });
var cube = new THREE.Mesh(geometry, material);
scene.add(cube);

// Lights
var ambient = new THREE.AmbientLight(0x808080);
scene.add(ambient);

var light1 = new THREE.DirectionalLight(0xffffff, 1.0);
light1.position.set(2, 3, 2);
scene.add(light1);

var light2 = new THREE.DirectionalLight(0x4466aa, 0.5);
light2.position.set(-2, -1, 1);
scene.add(light2);

console.log('Scene ready: cube + 2 directional lights');

// Animation loop
function animate() {
    requestAnimationFrame(animate);
    cube.rotation.x += 0.01;
    cube.rotation.y += 0.02;
    renderer.render(scene, camera);
}
animate();
