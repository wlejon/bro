// three.js test - GLTFLoader loading a .glb model
var canvas = document.getElementById('c');
console.log('THREE version:', THREE.REVISION);

var renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: false });
renderer.setSize(1024, 768, false);

var scene = new THREE.Scene();
scene.background = new THREE.Color(0x334455);

var camera = new THREE.PerspectiveCamera(75, 1024 / 768, 0.1, 1000);
camera.position.set(1.5, 1.5, 2.5);
camera.lookAt(0, 0, 0);

// Lighting
var ambientLight = new THREE.AmbientLight(0x404040, 2.0);
scene.add(ambientLight);

var dirLight = new THREE.DirectionalLight(0xffffff, 3.0);
dirLight.position.set(5, 5, 5);
scene.add(dirLight);

// Load glTF model
var loader = new THREE.GLTFLoader();
console.log('Loading cube.glb...');

var model = null;

loader.load('cube.glb',
    function(gltf) {
        console.log('glTF loaded successfully!');
        console.log('  scenes:', gltf.scenes.length);
        console.log('  animations:', gltf.animations.length);
        model = gltf.scene;
        scene.add(model);
    },
    function(progress) {
        // Progress callback
    },
    function(error) {
        console.error('glTF load error:', error);
    }
);

// Also add a reference cube so we can see something while model loads
var refCube = new THREE.Mesh(
    new THREE.BoxGeometry(0.3, 0.3, 0.3),
    new THREE.MeshStandardMaterial({ color: 0xff4444, roughness: 0.5 })
);
refCube.position.set(2, 0, 0);
scene.add(refCube);

console.log('Scene ready - GLTFLoader test');

function animate() {
    requestAnimationFrame(animate);
    if (model) {
        model.rotation.y += 0.01;
    }
    refCube.rotation.y += 0.02;
    renderer.render(scene, camera);
}
animate();
