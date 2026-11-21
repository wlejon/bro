// Test bro.eye.scene on live 3D SceneGraph + Vulkan depth/BVH spatial checks.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({
        fov: 55, near: 0.1, far: 100,
        position: [0, 3.2, 8.5],
        target: [0, 0.8, 0],
        up: [0, 1, 0],
    });

    scene.createLight({
        type: 'directional',
        direction: [-0.5, -1.0, -0.4],
        color: [1, 1, 1],
        intensity: 3.0,
    });

    // 1. Ground slab at y in [-0.2, 0]
    const ground = scene.createMesh({ mesh: 'box', color: '#3a424d' });
    ground.name = 'ground';
    ground.scale = [12, 0.2, 12];
    ground.position = [0, -0.1, 0];

    // 2. Grounded cube sitting cleanly at y in [0, 1]
    const grounded = scene.createMesh({ mesh: 'box', width: 1, height: 1, depth: 1, color: '#e64935' });
    grounded.name = 'grounded';
    grounded.position = [-2.0, 0.5, 0];

    // 3. Floating cube hovering 0.75 units above the ground
    const floater = scene.createMesh({ mesh: 'box', width: 1, height: 1, depth: 1, color: '#3b82f6' });
    floater.name = 'floater';
    floater.position = [0.0, 1.25, 0];

    // 4. Two interpenetrating cubes on the right
    const clipA = scene.createMesh({ mesh: 'box', width: 1, height: 1, depth: 1, color: '#10b981' });
    clipA.name = 'clipA';
    clipA.position = [2.2, 0.5, 0];

    const clipB = scene.createMesh({ mesh: 'box', width: 1, height: 1, depth: 1, color: '#f59e0b' });
    clipB.name = 'clipB';
    clipB.position = [2.6, 0.5, 0.2];

    const report = bro.eye.scene(scene, { focus: 'grounded', checkStability: true });
    assert(report.width === 256 && report.height === 256, 'snapshot dimensions match canvas');
    assert(report.skyRatio > 0.05 && report.skyRatio < 0.95, `skyRatio plausible (${report.skyRatio})`);
    assert(report.medianDist > 1.0 && report.nearDist > 0.5, 'depth distances reconstructed');
    assert(report.stability.tested === true, 'camera jitter stability tested');

    const groundedInfo = report.nodes.find(n => n.name === 'grounded');
    assert(groundedInfo && groundedInfo.screenCoverage > 0.005, 'grounded mesh visible in screen coverage');
    assert(groundedInfo.silhouetteContrast > 10.0, `grounded silhouette contrasts against bg (${groundedInfo.silhouetteContrast})`);
    assert(groundedInfo.frameClipped === false, 'grounded mesh is not clipped by viewport');
    assert(groundedInfo.formRange > 0.05, `grounded has positive interior formRange (${groundedInfo.formRange})`);

    const floatIss = report.issues.find(i => i.kind === 'floating' && i.nodeName === 'floater');
    assert(floatIss && Math.abs(floatIss.gap - 0.75) < 0.1,
        `expected floating gap ~0.75 on floater, got ${floatIss ? floatIss.gap : 'none'}`);

    const interIss = report.issues.find(i => i.kind === 'interpenetration');
    assert(interIss !== undefined, 'caught interpenetrating cubes');

    const os = require('os');
    const path = require('path');
    // Depth & normals visualizations
    const depthVis = bro.eye.visualize(scene, { mode: 'depth', path: path.join(os.tmpdir(), 'eye_test_depth.png').replace(/\\/g, '/') });
    assert(depthVis.panels.length === 2 && depthVis.width > 256, 'depth visualization created');
    const normVis = bro.eye.visualize(scene, { mode: 'normals' });
    assert(normVis.panels.length === 2, 'normals visualization created');
    const auditVis = bro.eye.visualize(scene, { mode: 'audit' });
    assert(auditVis.panels.length === 1, 'scene audit overlay created');

    // Framing clip detection: zoom camera tightly onto grounded cube so it touches viewport border.
    scene.setCamera({
        fov: 55, near: 0.1, far: 100,
        position: [-2.0, 0.5, 1.05],
        target: [-2.0, 0.5, 0],
        up: [0, 1, 0],
    });
    const clippedReport = bro.eye.scene(scene, { focus: 'grounded' });
    const groundedClipped = clippedReport.nodes.find(n => n.name === 'grounded');
    assert(groundedClipped && groundedClipped.frameClipped === true, 'close-up marks frameClipped');
    assert(clippedReport.issues.some(i => i.kind === 'frame-clipped'), 'emits frame-clipped warning');
}
