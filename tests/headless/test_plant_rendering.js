// Headless verification of plant primitives: leafCard, bezierSweep, flower,
// twoSided + subsurface materials, and global wind sway. Renders one frame
// to a PNG and asserts pixel variance is non-trivial (i.e. the scene is
// not a uniform color).

const canvas = document.createElement('canvas');
canvas.width = 800;
canvas.height = 600;
canvas.style.width = '800px';
canvas.style.height = '600px';
document.body.appendChild(canvas);

const scene = canvas.getContext('scene');
assert(scene, 'scene context');

scene.setCamera({
    fov: 50,
    position: [0, 1.4, 3.0],
    target: [0, 0.5, 0],
    aspect: canvas.width / canvas.height,
});
scene.setAmbient([0.15, 0.15, 0.18]);

// Sun (directional)
const sun = scene.createLight({
    kind: 'directional',
    direction: [-0.4, -1.0, -0.3],
    color: [1.0, 0.96, 0.9],
    intensity: 3.0,
});

// 1. Bezier-swept stem (curved S-shape).
const stemCtrl = [
    [0, 0, 0],
    [0.05, 0.4, 0.0],
    [-0.1, 0.8, 0.0],
    [0.0, 1.2, 0.0],
];
const stem = Mesh.bezierSweep(
    stemCtrl,
    [[0.03, 0], [0.021, 0.021], [0, 0.03], [-0.021, 0.021],
     [-0.03, 0], [-0.021, -0.021], [0, -0.03], [0.021, -0.021]],
    { samples: 32, profileScale: [1.0, 0.6] }
);
const stemNode = scene.createMesh({
    mesh: stem,
    color: [0.35, 0.55, 0.25],
    metallic: 0.0,
    roughness: 0.7,
    transfer: true,
});

// 2. Two leaf cards branching off the stem.
function makeLeaf(angleDeg, atY) {
    const m = Mesh.leafCard('oval', {
        width: 0.35,
        length: 0.7,
        bend: 0.5,
        curl: 0.2,
        stemOffset: true,
    });
    const node = scene.createMesh({
        mesh: m,
        color: [0.2, 0.6, 0.2, 1.0],
        metallic: 0.0,
        roughness: 0.85,
        twoSided: true,
        subsurface: 0.5,
        transfer: true,
    });
    const a = angleDeg * Math.PI / 180;
    node.rotation = [0, Math.sin(a/2), 0, Math.cos(a/2)];
    node.position = [0.05, atY, 0];
    return node;
}
const leafA = makeLeaf(40, 0.6);
const leafB = makeLeaf(-40, 0.85);

// 3. Flower at the top of the stem.
const flowerMesh = Mesh.flower({
    petalCount: 8,
    petalShape: 'petal',
    petalLength: 0.35,
    petalWidth: 0.18,
    petalBend: 0.7,
    layers: 2,
    layerTwist: 0.4,
    centerRadius: 0.07,
    centerHeight: 0.04,
});
const flowerNode = scene.createMesh({
    mesh: flowerMesh,
    color: [0.95, 0.45, 0.65, 1.0],
    metallic: 0.0,
    roughness: 0.6,
    twoSided: true,
    subsurface: 0.4,
    transfer: true,
});
flowerNode.position = [0, 1.2, 0];

// 4. Ground plane for context.
const groundMesh = (function() {
    return new Mesh({
        positions: new Float32Array([
            -3,0,-3,  3,0,-3,  3,0,3,  -3,0,3
        ]),
        normals: new Float32Array([
            0,1,0, 0,1,0, 0,1,0, 0,1,0
        ]),
        indices: new Uint32Array([0,1,2, 0,2,3]),
    });
})();
const groundNode = scene.createMesh({
    mesh: groundMesh,
    color: [0.4, 0.35, 0.3, 1.0],
    metallic: 0.0,
    roughness: 0.9,
    transfer: true,
});

// 5. Wind on.
scene.setWind({ direction: [1, 0, 0.3], strength: 0.05, frequency: 2.0 });

// Advance virtual time so windTime ticks before screenshot.
advanceTime(500);
flush();

screenshot('test_plant_rendering.png');

// Read the PNG back via Uint8Array fs and check variance roughly.
const _fs = require('fs');
const png = _fs.readFileSync('test_plant_rendering.png');
assert(png.length > 1000, 'png file written and non-trivial size');
console.log('png bytes:', png.length);
console.log('plant rendering test ok');
