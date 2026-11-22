// Tests bro.eye.structure over InstancedMeshNode with spatial filtering (near/bounds),
// zero-scale pooled slot skipping, ignoreFloating, watchPairs, and overhang detection.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '256');
canvas.setAttribute('height', '256');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    const floor = scene.createMesh({ mesh: 'box', width: 60, height: 0.2, depth: 60, color: '#3a424d' });
    floor.name = 'Floor';
    floor.position = [0, -0.1, 0];

    // Instanced pillars: grounded, floating, distant floating, zero-scale unused pool slot
    const pillars = scene.createInstancedMesh({ mesh: Mesh.box(0.3, 0.5, 0.3), color: '#8899aa' });
    pillars.name = 'PoolPillars';
    pillars.setInstances(new Float32Array([
        1, 0, 0, -2.0,  0, 1, 0, 0.5,  0, 0, 1, 0.0,  1, 1, 1, 1, // #0 grounded local
        1, 0, 0,  0.0,  0, 1, 0, 1.8,  0, 0, 1, 0.0,  1, 1, 1, 1, // #1 floating local
        1, 0, 0, 18.0,  0, 1, 0, 2.5,  0, 0, 1, 0.0,  1, 1, 1, 1, // #2 floating distant
        0, 0, 0,  0.0,  0, 0, 0, 0.0,  0, 0, 0, 0.0,  1, 1, 1, 1, // #3 zero-scale pool dummy
    ]));

    // Raised ledge + overhanging instanced post (50% off ledge)
    const ledge = scene.createMesh({ mesh: Mesh.box(0.5, 0.2, 0.5), color: '#667788' });
    ledge.name = 'Ledge';
    ledge.position = [2.5, 0.2, 0];

    const post = scene.createInstancedMesh({ mesh: Mesh.box(0.3, 0.4, 0.3), color: '#aa7755' });
    post.name = 'OverhangPost';
    post.setInstances(new Float32Array([
        1, 0, 0, 2.5,  0, 1, 0, 0.8,  0, 0, 1, 0.5,  1, 1, 1, 1,
    ]));

    // Watch pair on instanced gears
    const gearA = scene.createInstancedMesh({ mesh: Mesh.box(0.2, 0.2, 0.1) });
    gearA.name = 'GearA';
    gearA.setInstances(new Float32Array([
        1, 0, 0, 0.0,  0, 1, 0, 0.2,  0, 0, 1, 2.0,  1, 1, 1, 1,
    ]));
    const gearB = scene.createInstancedMesh({ mesh: Mesh.box(0.2, 0.2, 0.1) });
    gearB.name = 'GearB';
    gearB.setInstances(new Float32Array([
        1, 0, 0, 0.6,  0, 1, 0, 0.2,  0, 0, 1, 2.0,  1, 1, 1, 1,
    ]));
    flush();

    const nearRes = bro.eye.structure(scene, {
        near: { center: [0, 0, 0], radius: 5.0 },
        watchPairs: [['GearA#0', 'GearB#0']],
    });
    assert(nearRes.ok === false, 'near structure audit detects local defects');
    assert(nearRes.instancedMeshCount === 5,
        'near audit counts 5 active local instances (skips zero-scale & distant #2), got ' + nearRes.instancedMeshCount);
    assert(nearRes.floatingCount === 1, 'near audit catches 1 local floating instance, got ' + nearRes.floatingCount);
    assert(nearRes.overhangCount === 1, 'near audit catches 1 overhanging instance, got ' + nearRes.overhangCount);
    assert(nearRes.gapCount === 1, 'near audit catches watchPair gap on instanced meshes');

    const boundsOk = bro.eye.structure(scene, {
        bounds: { min: [-3.0, -0.5, -1.0], max: [-1.0, 2.0, 1.0] },
    });
    assert(boundsOk.ok === true && boundsOk.instancedMeshCount === 1,
        'bounds filter isolates clean grounded PoolPillars#0');

    const ignoredRes = bro.eye.structure(scene, {
        near: { center: [0, 0, 0], radius: 5.0 },
        ignoreFloating: ['PoolPillars#1', 'OverhangPost'],
    });
    assert(ignoredRes.floatingCount === 0 && ignoredRes.overhangCount === 0,
        'ignoreFloating suppresses ignored instances');

    // Non-uniformly Y-scaled unit boxes (sy=0.05 ledge supporting sy=0.1 overhanging post)
    const scaledLedge = scene.createInstancedMesh({ mesh: Mesh.box(0.5, 0.5, 0.5), color: '#556677' });
    scaledLedge.name = 'ScaledLedge';
    scaledLedge.setInstances(new Float32Array([
        4.0, 0, 0, 10.0,  0, 0.05, 0, 1.0,  0, 0, 4.0, 0.0,  1, 1, 1, 1,
    ]));
    const scaledPost = scene.createInstancedMesh({ mesh: Mesh.box(0.5, 1.0, 0.5), color: '#bb8855' });
    scaledPost.name = 'ScaledOverhangPost';
    scaledPost.setInstances(new Float32Array([
        0.3, 0, 0, 10.0,  0, 0.1, 0, 1.125,  0, 0, 0.3, 2.0,  1, 1, 1, 1,
    ]));
    flush();

    const scaledRes = bro.eye.structure(scene, {
        near: { center: [10, 1, 0], radius: 5.0 },
        ignoreFloating: ['ScaledLedge'],
    });
    assert(scaledRes.ok === false && scaledRes.overhangCount >= 1,
        'catches overhang on non-uniformly scaled sy=0.05 ledge + sy=0.1 post');

    console.log('OK test_eye_structure_instanced: instanced=' + nearRes.instancedMeshCount);
}
