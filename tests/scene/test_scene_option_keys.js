// Factory option keys and getters the bronze port had dropped:
//   createInstancedMesh: staticBatch, atlasCols / atlasRows (+ getters), unlit
//   createShape: shape, cornerRadius, anchorX / anchorY, stroke, points
//   createSprite: anchorX / anchorY, name
//   createHtmlNode: rotation, scale, pxPerUnit, worldAnchor, billboard, name
//   PhysicsNode.bodyId

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
    scene.setCamera({ fov: 60, near: 0.1, far: 200, position: [0, 5, 10], target: [0, 0, 0], up: [0, 1, 0] });

    // --- InstancedMesh ---------------------------------------------------------
    const im = scene.createInstancedMesh({
        mesh: Mesh.box(0.5, 0.5, 0.5), staticBatch: true, atlasCols: 4, atlasRows: 2,
        unlit: true, color: '#ff0000',
        instances: new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
                                     1,0,0,0, 0,1,0,0, 0,0,1,0, 2,0,0,1]),
    });
    assert(im && im.type === 'instancedMesh', 'instanced node');
    assert(im.staticBatch === true, 'staticBatch option reaches the node');
    assert(im.atlasCols === 4 && im.atlasRows === 2,
           'atlasCols/atlasRows option + getters: ' + im.atlasCols + 'x' + im.atlasRows);
    assert(im.instanceCount === 2, 'instances still applied, got ' + im.instanceCount);
    im.staticBatch = false;
    assert(im.staticBatch === false, 'staticBatch round-trips');
    im.setAtlasGrid(3, 5);
    assert(im.atlasCols === 3 && im.atlasRows === 5, 'getters follow setAtlasGrid');
    const onlyCols = scene.createInstancedMesh({ mesh: 'box', atlasCols: 6 });
    assert(onlyCols.atlasCols === 6 && onlyCols.atlasRows === 1, 'a lone atlasCols defaults rows to 1');
    const plain = scene.createInstancedMesh({ mesh: 'box' });
    assert(plain.staticBatch === false && plain.atlasCols === 1, 'defaults: no batch, 1x1 atlas');
    const m = scene.createMesh({ mesh: 'box' });
    assert(m.atlasCols === 0 && m.staticBatch === undefined, 'instanced-only members are inert on a mesh');

    // --- Shape -------------------------------------------------------------------
    const sh = scene.createShape({ shape: 'roundrect', width: 80, height: 40, cornerRadius: 12,
                                   anchorX: 0, anchorY: 1, fill: '#00ff00', stroke: '#000000', strokeWidth: 2 });
    assert(near(sh.cornerRadius, 12), 'cornerRadius option: ' + sh.cornerRadius);
    assert(near(sh.anchorX, 0) && near(sh.anchorY, 1), 'shape anchor option: ' + sh.anchorX + ',' + sh.anchorY);
    assert(near(sh.strokeWidth, 2), 'strokeWidth option');
    sh.anchorX = 0.25;
    assert(near(sh.anchorX, 0.25) && near(sh.anchorY, 1), 'anchorX setter keeps anchorY');
    sh.cornerRadius = 3;
    assert(near(sh.cornerRadius, 3), 'cornerRadius round-trips');
    const circ = scene.createShape({ shape: 'circle', radius: 9 });
    assert(near(circ.radius, 9), 'circle radius option');
    const defAnchor = scene.createShape({ width: 10, height: 10 });
    assert(near(defAnchor.anchorX, 0.5) && near(defAnchor.anchorY, 0.5), 'anchor defaults to center');

    // --- Sprite ------------------------------------------------------------------
    const sp = scene.createSprite({ name: 'hero', width: 32, height: 32, anchorX: 0.5, anchorY: 0 });
    assert(sp.name === 'hero', 'sprite name option: ' + sp.name);
    assert(near(sp.anchorX, 0.5) && near(sp.anchorY, 0), 'sprite anchor option: ' + sp.anchorX + ',' + sp.anchorY);

    // --- HtmlNode ------------------------------------------------------------------
    const h = Math.SQRT1_2;
    const hn = scene.createHtmlNode({
        html: '<div>hi</div>', width: 120, height: 40, name: 'label',
        rotation: [0, h, 0, h], scale: [2, 3, 4], pxPerUnit: 50,
        worldAnchor: [1, 2, 3], billboard: 'ylock',
    });
    assert(hn.type === 'html', 'html node');
    assert(hn.name === 'label', 'html name option: ' + hn.name);
    const q = hn.quaternion;
    assert(near(Math.abs(q[1]), h) && near(Math.abs(q[3]), h), 'html rotation option: ' + JSON.stringify(q));
    const s = hn.scale;
    assert(near(s[0], 2) && near(s[1], 3) && near(s[2], 4), 'html scale option: ' + JSON.stringify(s));
    assert(near(hn.pxPerUnit, 50), 'pxPerUnit option: ' + hn.pxPerUnit);
    const wa = hn.worldAnchor;
    assert(Array.isArray(wa) && near(wa[0], 1) && near(wa[1], 2) && near(wa[2], 3),
           'worldAnchor option: ' + JSON.stringify(wa));
    assert(hn.billboard === 'ylock', 'billboard option: ' + hn.billboard);
    const hn2 = scene.createHtmlNode({ html: '<b>x</b>', rotation: 0.5, scale: 1.5 });
    assert(near(hn2.rotationZ, 0.5), 'numeric rotation is Z: ' + hn2.rotationZ);
    assert(near(hn2.scale[0], 1.5) && near(hn2.scale[2], 1.5), 'numeric scale is uniform');
    hn2.pxPerUnit = 25;
    assert(near(hn2.pxPerUnit, 25), 'pxPerUnit round-trips');
    assert(m.pxPerUnit === undefined, 'pxPerUnit is undefined off html nodes');

    // --- PhysicsNode.bodyId ------------------------------------------------------------
    const pn = scene.createPhysicsNode({});
    if (pn) {
        assert(pn.bodyId === null, 'no body: bodyId is null, got ' + pn.bodyId);
        const pn2 = scene.createPhysicsNode({ bodyId: 7 });
        assert(pn2.bodyId === 7, 'bodyId reads the body the node was given, got ' + pn2.bodyId);
    }
    assert(m.bodyId === undefined, 'bodyId is undefined off physics nodes');

    const shot = scene.captureFrame();
    assert(shot && shot.width > 0, 'a frame with these nodes renders');
    console.log('scene option keys OK');
}
document.body.removeChild(canvas);
