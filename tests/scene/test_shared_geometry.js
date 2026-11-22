// Shared geometry (scene.createGeometry, createMesh({ geometry })): many
// nodes draw one mesh, uploaded once. Proof by the upload counter
// (__host.sceneUploadStats) and by pixels: the first node uploads it, a
// hundred more and a node destroyed and made again upload nothing; each
// draws where it stands; a released geometry still draws for the nodes
// holding it; a node's raycast hits it; and a plant's vegetation records
// come with it.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

function brightAt(scn, img, x, y, z) {
    const p = scn.projectLocal(x, y, z);
    assert(p && !p.behind, `(${x}, ${y}, ${z}) is on screen`);
    const i = (Math.round(p.y) * img.width + Math.round(p.x)) * 4;
    return img.data[i];
}

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    const s = freshScene(256);
    const scn = s.scene;
    scn.setCamera({ fov: 50, near: 0.1, far: 200, position: [0, 30, 30], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient([0, 0, 0]);
    const RED = { color: [1, 0, 0], emissive: 2, emissiveColor: [1, 0, 0], castsShadow: false };

    const mesh = Mesh.box(0.4, 0.4, 0.4);
    const geo = scn.createGeometry(mesh);
    assert(typeof geo.id === 'number' && geo.id > 0, `createGeometry gives a handle (${geo.id})`);

    const u0 = __host.sceneUploadStats().meshUploads;
    const first = scn.createMesh(Object.assign({ geometry: geo, position: [-6, 0, 0] }, RED));
    scn.captureFrame();
    const u1 = __host.sceneUploadStats().meshUploads;
    assert(u1 - u0 === 1, `the first node uploads the geometry once (${u1 - u0})`);

    const many = [];
    for (let i = 0; i < 100; i++)
        many.push(scn.createMesh(Object.assign({ geometry: geo, position: [(i % 10) - 4.5, 0, Math.floor(i / 10) - 4.5] }, RED)));
    let img = scn.captureFrame();
    const u2 = __host.sceneUploadStats().meshUploads;
    assert(u2 === u1, `a hundred more nodes upload nothing (${u2 - u1})`);
    assert(brightAt(scn, img, -6, 0.4, 0) > 150, 'the first node draws where it stands');
    assert(brightAt(scn, img, 4.5, 0.4, 4.5) > 150, 'the last node draws where it stands');
    assert(brightAt(scn, img, 8, 0, 8) < 30, 'and nothing is drawn between them and the edge');

    // A node destroyed and made again (a level of detail swapped back).
    many[0].destroy();
    many[0] = scn.createMesh(Object.assign({ geometry: geo, position: [-4.5, 0, -4.5] }, RED));
    scn.captureFrame();
    assert(__host.sceneUploadStats().meshUploads === u2, 'a node made again uploads nothing');

    // Released: the nodes holding it keep drawing it; a new node cannot have it.
    geo.release();
    img = scn.captureFrame();
    assert(brightAt(scn, img, -6, 0.4, 0) > 150, 'a released geometry still draws for its nodes');
    assert(__host.sceneUploadStats().meshUploads === u2, 'and is not uploaded again');

    // Raycast and bounds come from the shared mesh.
    const hit = scn.raycast([-6, 10, 0], [0, -1, 0], 50);
    assert(hit && Math.abs(hit.point[1] - 0.4) < 0.02, `a ray down hits the shared box's top (${hit && hit.point[1]})`);

    // Vegetation records ride with it: a plant node from shared geometry sways.
    const card = new Mesh({
        positions: new Float32Array([-0.5, 1, 0, 0.5, 1, 0, 0.5, 2, 0, -0.5, 2, 0]),
        indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
        normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
    });
    const veg = new Float32Array(4 * 16);
    for (let v = 0; v < 4; v++) veg.set([0, 0, 0, 2, 0, 1.5, 0, 0.5, 0, 1, 0, 1, 0, 0, 0, 0], v * 16);
    card.vegetation = veg;
    const plantGeo = scn.createGeometry(card);
    const plant = scn.createMesh({ geometry: plantGeo, position: [6, 0, 6], doubleSided: true,
        color: [0, 1, 0], emissive: 2, emissiveColor: [0, 1, 0], castsShadow: false });
    img = scn.captureFrame();
    const p = scn.projectLocal(6, 1.5, 6);
    const g = img.data[(Math.round(p.y) * img.width + Math.round(p.x)) * 4 + 1];
    assert(g > 150, `a plant from shared geometry (vegetation records and all) draws (${g})`);

    for (const n of many) n.destroy();
    first.destroy();
    plant.destroy();
    plantGeo.release();
    scn.captureFrame();
    document.body.removeChild(s.canvas);
    flush();
}
