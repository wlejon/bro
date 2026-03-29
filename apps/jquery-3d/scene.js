// Three.js scene — spinning cubes with lighting
var canvas = document.getElementById('c');
var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: true });
renderer.setSize(1024, 768, false);

var scene = new THREE.Scene();
scene.background = new THREE.Color(0x1a1a2e);

var camera = new THREE.PerspectiveCamera(60, 1024 / 768, 0.1, 100);
camera.position.set(0, 2, 5);
camera.lookAt(0, 0, 0);

// Lighting
scene.add(new THREE.AmbientLight(0x404060, 1.5));
var dirLight = new THREE.DirectionalLight(0xffffff, 2.0);
dirLight.position.set(3, 5, 4);
scene.add(dirLight);

var pointLight = new THREE.PointLight(0xe94560, 2.0, 10);
pointLight.position.set(-2, 1, 2);
scene.add(pointLight);

// Create a grid of cubes
var cubes = [];
var colors = [0xe94560, 0x0fbcf9, 0x0be881, 0xffc048, 0xa855f7, 0xf97316];
for (var i = 0; i < 12; i++) {
    var geo = new THREE.BoxGeometry(0.4, 0.4, 0.4);
    var mat = new THREE.MeshStandardMaterial({
        color: colors[i % colors.length],
        roughness: 0.4,
        metalness: 0.3
    });
    var cube = new THREE.Mesh(geo, mat);
    var angle = (i / 12) * Math.PI * 2;
    cube.position.set(Math.cos(angle) * 2, Math.sin(i * 0.5) * 0.5, Math.sin(angle) * 2);
    scene.add(cube);
    cubes.push(cube);
}

// Floor grid
var gridGeo = new THREE.PlaneGeometry(8, 8, 8, 8);
var gridMat = new THREE.MeshStandardMaterial({
    color: 0x2a2a4e,
    roughness: 0.9,
    wireframe: true
});
var grid = new THREE.Mesh(gridGeo, gridMat);
grid.rotation.x = -Math.PI / 2;
grid.position.y = -1;
scene.add(grid);

console.log('Three.js scene ready (' + cubes.length + ' cubes)');

function animateScene() {
    requestAnimationFrame(animateScene);

    // Clear Three.js internal GL state cache — the engine's UI compositor
    // changes GL state between frames, so Three.js must re-bind everything.
    renderer.state.reset();

    var t = performance.now() * 0.001;
    for (var i = 0; i < cubes.length; i++) {
        cubes[i].rotation.x = t * (0.5 + i * 0.1);
        cubes[i].rotation.y = t * (0.3 + i * 0.15);
        var angle = (i / 12) * Math.PI * 2 + t * 0.3;
        cubes[i].position.x = Math.cos(angle) * 2;
        cubes[i].position.z = Math.sin(angle) * 2;
        cubes[i].position.y = Math.sin(t * 2 + i) * 0.5;
    }

    pointLight.position.x = Math.cos(t) * 3;
    pointLight.position.z = Math.sin(t) * 3;

    camera.position.x = Math.cos(t * 0.2) * 5;
    camera.position.z = Math.sin(t * 0.2) * 5;
    camera.lookAt(0, 0, 0);

    renderer.render(scene, camera);
}
animateScene();
