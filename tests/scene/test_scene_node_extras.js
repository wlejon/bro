// The SceneNode members restored after the bronze port (js/scene_extras.js
// over native_scene_extras.cpp): the per-type accessors, the instanced-mesh
// operations, syncToPhysics, and the scalar lookAt(x, y, z) form.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '128');
canvas.setAttribute('height', '128');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    console.log('scene context not available (no GPU); skipping');
} else {
    const near = (a, b, eps) => Math.abs(a - b) < (eps || 1e-3);

    // --- lookAt: scalar and array forms agree ------------------------------
    const a = scene.createMesh({ mesh: 'box' });
    const b = scene.createMesh({ mesh: 'box' });
    a.position = [0, 0, 0];
    b.position = [0, 0, 0];
    a.lookAt(3, 1, -2);
    b.lookAt([3, 1, -2]);
    const qa = a.quaternion, qb = b.quaternion;
    assert(Array.isArray(qa) && qa.length === 4, 'quaternion reads back as [x,y,z,w]');
    for (let i = 0; i < 4; i++) assert(near(qa[i], qb[i]), 'lookAt(x,y,z) equals lookAt([x,y,z]) at ' + i);

    // --- Mesh accessors ------------------------------------------------------
    const mesh = scene.createMesh({ mesh: 'sphere', color: '#ffffff' });
    assert(mesh.type === 'mesh', 'mesh node');
    mesh.alphaCutoff = 0.4;
    assert(near(mesh.alphaCutoff, 0.4), 'alphaCutoff round-trips');
    mesh.setAlphaCutoff(0.6);
    assert(near(mesh.alphaCutoff, 0.6), 'setAlphaCutoff() sets the accessor');
    mesh.nearClipDist = 2.5;
    assert(near(mesh.nearClipDist, 2.5), 'nearClipDist round-trips');
    mesh.emissiveColor = [0.25, 0.5, 1.0];
    let ec = mesh.emissiveColor;
    assert(Array.isArray(ec) && near(ec[0], 0.25) && near(ec[1], 0.5) && near(ec[2], 1.0),
           'emissiveColor array round-trips, got ' + JSON.stringify(ec));
    mesh.emissiveColor = '#ff8000';
    ec = mesh.emissiveColor;
    assert(near(ec[0], 1.0, 0.01) && near(ec[1], 128 / 255, 0.01) && near(ec[2], 0, 0.01),
           'emissiveColor CSS string parses, got ' + JSON.stringify(ec));
    const viaOpts = scene.createMesh({ mesh: 'box', emissiveColor: [0, 1, 0], alphaCutoff: 0.3 });
    assert(near(viaOpts.emissiveColor[1], 1) && near(viaOpts.alphaCutoff, 0.3), 'createMesh options feed the accessors');
    assert(mesh.radius === undefined, 'radius is undefined off a Shape');
    assert(mesh.doubleSided === undefined, 'doubleSided is undefined off an InstancedMesh');
    mesh.billboard = 'ylock';
    assert(mesh.billboard === 'ylock', 'billboard round-trips ylock');
    mesh.billboard = 'full';
    assert(mesh.billboard === 'full', 'billboard round-trips full');

    // --- Shape accessors -----------------------------------------------------
    const shape = scene.createShape({ type: 'circle', radius: 3 });
    assert(shape.type === 'shape', 'shape node');
    assert(near(shape.radius, 3), 'radius from options, got ' + shape.radius);
    shape.radius = 7;
    assert(near(shape.radius, 7), 'radius round-trips');
    shape.fillColor = 'rgb(255, 0, 0)';
    assert(shape.fillColor === 'rgba(255,0,0,1.00)', 'fillColor reads back rgba(), got ' + shape.fillColor);
    shape.strokeColor = '#00ff0080';
    assert(shape.strokeColor.indexOf('rgba(0,255,0,') === 0, 'strokeColor reads back rgba(), got ' + shape.strokeColor);
    shape.strokeWidth = 2.5;
    assert(near(shape.strokeWidth, 2.5), 'strokeWidth round-trips');

    // --- Sprite: frameIndex ----------------------------------------------------
    const sprite = scene.createSprite({});
    if (sprite) {
        sprite.frameIndex = 3;
        assert(sprite.frameIndex === 3, 'frameIndex round-trips');
    }

    // --- ReflectionProbe: interior / priority -----------------------------------
    const probe = scene.createReflectionProbe({ size: 4, updateMode: 'manual' });
    probe.interior = 1.5;
    assert(near(probe.interior, 1.5), 'interior round-trips');
    probe.priority = 7;
    assert(probe.priority === 7, 'priority round-trips');

    // --- InstancedMesh operations ------------------------------------------------
    const inst = scene.createInstancedMesh({ mesh: 'box' });
    assert(inst.type === 'instancedMesh', 'instanced node');
    inst.doubleSided = true;
    assert(inst.doubleSided === true, 'doubleSided round-trips');
    inst.setDoubleSided(false);
    assert(inst.doubleSided === false, 'setDoubleSided() sets the accessor');
    inst.alphaCutoff = 0.2;
    assert(near(inst.alphaCutoff, 0.2), 'alphaCutoff on an instanced mesh');
    inst.emissiveColor = [1, 0, 0];
    assert(near(inst.emissiveColor[0], 1), 'emissiveColor on an instanced mesh');
    inst.setAtlasGrid(4, 2);

    const I = new Float32Array(32);
    for (let k = 0; k < 2; k++) { I[k * 16 + 0] = 1; I[k * 16 + 5] = 1; I[k * 16 + 10] = 1; I[k * 16 + 15] = 1; I[k * 16 + 12] = k * 2; }
    inst.updateInstances(I);
    assert(inst.instanceCount === 2, 'updateInstances() uploads, count ' + inst.instanceCount);
    const M = new Float32Array(16); M[0] = M[5] = M[10] = M[15] = 1; M[12] = 9;
    inst.updateInstance(1, M);
    assert(inst.instanceCount === 2, 'updateInstance keeps the count');
    let threw = false;
    try { inst.updateInstance(0, [1, 2, 3]); } catch (e) { threw = true; }
    assert(threw, 'updateInstance with fewer than 16 floats throws');

    inst.setInstancedMesh({
        positions: new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]),
        indices: new Uint32Array([0, 1, 2]),
    });
    threw = false;
    try { mesh.setInstancedMesh({ positions: new Float32Array(9), indices: new Uint32Array(3) }); } catch (e) { threw = true; }
    assert(threw, 'setInstancedMesh on a plain mesh throws');

    // scatter / tube: one segment each, bounds derived.
    const scatterNode = scene.createInstancedMesh({ mesh: 'box' });
    scatterNode.setScatterSegments({
        segments: new Float32Array([0, 0, 0, 0.1, 0, 1, 0, 0.05]),
        instSeg: new Float32Array([0, 0, 0]),
        seed: 3, baseScale: 0.5,
    });
    assert(scatterNode.isScatter === true, 'setScatterSegments enters scatter mode');
    const tubeNode = scene.createInstancedMesh({ tube: { segments: [0, 0, 0, 0.2, 0, 2, 0, 0.1], sides: 8 } });
    assert(tubeNode.isTube === true, 'createInstancedMesh({tube}) enters tube mode');

    // --- Physics node: autoSync / pixelsPerUnit / syncToPhysics --------------------
    const pn = scene.createPhysicsNode({});
    if (pn) {
        pn.autoSync = false;
        assert(pn.autoSync === false, 'autoSync round-trips');
        pn.pixelsPerUnit = 50;
        assert(near(pn.pixelsPerUnit, 50), 'pixelsPerUnit round-trips');
        pn.syncToPhysics();   // no world attached: a no-op, must not throw
    }
    assert(typeof mesh.syncToPhysics === 'function', 'syncToPhysics is on every node');

    // A frame with every node kind above in it must still render.
    const shot = scene.captureFrame();
    assert(shot && shot.width > 0, 'captureFrame with the decorated nodes renders');
    console.log('scene node extras OK');
}
