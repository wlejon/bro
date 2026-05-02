const canvas = document.getElementById('stage');
const scene = canvas.getContext('scene');

scene.setCamera({
    position: [3, 2.5, 4],
    target: [0, 0, 0],
    fov: 55,
});

scene.createLight({
    type: 'directional',
    direction: [-0.4, -1, -0.3],
    color: [1, 0.96, 0.9],
    intensity: 1.2,
});

scene.createLight({
    type: 'point',
    position: [-3, 2, -2],
    color: [0.4, 0.5, 1],
    intensity: 8,
    range: 12,
});

const cube = scene.createMesh({
    mesh: 'box',
    color: '#d68b4a',
    metallic: 0.1,
    roughness: 0.5,
});

scene.createMesh({
    mesh: 'plane',
    halfW: 6,
    halfD: 6,
    y: -0.6,
    color: '#1f2230',
    roughness: 0.9,
});

let t = 0;
function frame() {
    t += 0.016;
    cube.rx = Math.sin(t * 0.7) * 20;
    cube.ry = (cube.ry || 0) + 0.6;
    cube.rz = Math.cos(t * 0.5) * 10;
    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
