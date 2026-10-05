// Fixture for test_depth_policy.js: one frame of a scene that leans on the
// camera depth policy everywhere — MSAA (the depth resolve), SSAO and depth
// of field (the depth snapshot), a translucent quad (depth test without
// write) — printed as a coarse grid of RGBA samples on one line.
const canvas = document.createElement('canvas');
canvas.setAttribute('width', '96');
canvas.setAttribute('height', '96');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
scene.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0.5, 5], target: [0, 0, 0] });
scene.setMSAA(4);
scene.setSSAO({ enabled: true, radius: 1.0, intensity: 1.0, bias: 0.02 });
scene.setDepthOfField({ enabled: true, focusDistance: 5, focusRange: 2, maxBlur: 2 });
scene.createLight({ type: 'directional', direction: [-0.3, -1, -0.5], intensity: 2 });
scene.setAmbient([0.4, 0.4, 0.4]);
scene.createMesh({ mesh: Mesh.box(6, 6, 0.2), color: [0.1, 0.2, 0.9, 1], z: -2 });   // far wall
scene.createMesh({ mesh: Mesh.box(1, 1, 1), color: [0.9, 0.15, 0.1, 1], z: 0 });     // near cube
scene.createMesh({ mesh: Mesh.box(1, 1, 0.05), color: [0.1, 0.9, 0.1, 0.5], x: 1.2, z: 1 });

const img = scene.captureFrame();
const grid = [];
for (let y = 6; y < img.height; y += 12) {
    for (let x = 6; x < img.width; x += 12) {
        const i = (y * img.width + x) * 4;
        grid.push(img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]);
    }
}
const c = ((img.height >> 1) * img.width + (img.width >> 1)) * 4;
console.log('DEPTH_POLICY_FRAME ' + JSON.stringify({ centre: Array.from(img.data.slice(c, c + 4)), grid }));
