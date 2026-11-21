function assert(cond, msg) {
    if (!cond) throw new Error('ASSERT FAILED: ' + msg);
}

// 1. Offline Mesh geometric form critique
const sphere = Mesh.sphere(1.0, 28, 20);
const box = Mesh.box(1.0, 1.0, 1.0);
const fSphere = bro.eye.form(sphere, { azimuths: 6, slices: 6 });
const fBox = bro.eye.form(box, { azimuths: 6, slices: 6 });

assert(fSphere.ok && fSphere.surface.creaseEdgeCount === 0, 'sphere has zero dihedral creases');
assert(fSphere.surface.smoothnessScore > 0.85, 'sphere smoothnessScore high: ' + fSphere.surface.smoothnessScore);
assert(fBox.surface.creaseEdgeCount >= 12, 'box has >= 12 sharp edges');
assert(fSphere.surface.smoothnessScore > fBox.surface.smoothnessScore, 'sphere smoother than cube');
assert(fSphere.mass.grounded && fSphere.mass.symmetryScore > 0.9, 'sphere grounded & symmetric');
assert(fSphere.slices.length === 6 && fSphere.silhouettes.length === 6, 'returns 6 slices & 6 turntable silhouettes');

// 2. Live 3D SceneGraph sculptural lighting & diagnostic visualizers
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
        fov: 45, near: 0.1, far: 100,
        position: [0.0, 0.8, 4.2],
        target: [0.0, 0.2, 0.0],
        up: [0, 1, 0],
    });
    const keyLight = scene.createLight({
        type: 'directional',
        direction: [-0.7, -0.5, -0.5],
        color: [1.0, 0.96, 0.90],
        intensity: 3.5,
    });
    const rimLight = scene.createLight({
        type: 'directional',
        direction: [0.8, 0.2, 0.6],
        color: [0.75, 0.85, 1.0],
        intensity: 1.4,
    });

    const sculpture = scene.createMesh({
        data: sphere,
        color: '#ebe6de',
        roughness: 0.42,
    });
    sculpture.name = 'marble_study';
    sculpture.position = [0, 0.2, 0];

    const fScene = bro.eye.form(scene, { focus: 'marble_study', azimuths: 4 });
    assert(fScene.ok && fScene.subjectName === 'marble_study', 'scene form focuses marble_study');
    assert(fScene.lighting.analyzed === true, 'scene lighting analysis ran');
    assert(fScene.lighting.valueTurnScore > 0.35 && fScene.lighting.halftoneShare > 0.2,
        'sculptural key+rim produces value modeling turn: ' + JSON.stringify(fScene.lighting));

    const vZeb = bro.eye.visualize(scene, { mode: 'zebra', maxPanelDim: 320, zebraFreq: 10 });
    assert(vZeb.panels.length === 2 && vZeb.width > 100, 'zebra mode renders 2 panels');

    const vCurv = bro.eye.visualize(scene, { mode: 'curvature', maxPanelDim: 320 });
    assert(vCurv.panels.length === 2 && vCurv.width > 100, 'curvature mode renders 2 panels');

    const vTurn = bro.eye.visualize(scene, { mode: 'turntable', maxPanelDim: 240 });
    assert(vTurn.panels.length === 6 && vTurn.width > 100, 'turntable renders 6-panel sculptor contact sheet');

    const vRake = bro.eye.visualize(scene, { mode: 'raking', maxPanelDim: 240 });
    assert(vRake.panels.length === 4 && vRake.width > 100, 'raking renders 4-light diagnostic grid');

    // Confirm temporary raking lights cleaned up and original lights restored
    assert(keyLight.visible === true && rimLight.visible === true, 'original lights remain visible after raking');
}

console.log('test_eye_form PASS');
