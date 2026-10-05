// A SceneGraph moved to another canvas keeps everything it uploaded
// (docs/scene-api.js, "Moving a scene to another canvas"): scene.attachTo,
// scene.detach, scene.attached and scene.keepAlive.
//
// Proof is by counter, not by image: __host.sceneUploadStats() counts every
// mesh geometry upload and material texture upload the scene makes
// (scene/gpu_upload_stats.h). A move that rebuilt or re-uploaded the world
// would move those counters; this test asserts it does not, and that the
// scene composites on the new canvas (its FBO texture on the new element).

function makeCanvas() {
    const c = document.createElement('canvas');
    c.setAttribute('width', '160');
    c.setAttribute('height', '120');
    c.style.width = '160px';
    c.style.height = '120px';
    document.body.appendChild(c);
    return c;
}

const c1 = makeCanvas();
flush();
const scene = c1.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    const before = __host.sceneUploadStats();
    const tex = { data: new Uint8Array(8 * 8 * 4).fill(200), width: 8, height: 8 };
    for (let i = 0; i < 12; i++) {
        const m = scene.createMesh({ mesh: i % 2 ? 'box' : 'sphere', color: 'orange' });
        m.position = [(i - 6) * 0.25, 0, -8];  // all in view: uploads are lazy, at first draw
        if (i < 4) m.setBaseColorTexture(tex);
    }
    scene.createInstancedMesh({ mesh: 'box' }).setInstances(new Float32Array(16 * 3).map((_, k) => (k % 5 === 0 ? 1 : 0)));
    flush();
    flush();
    scene.captureFrame();
    const built = __host.sceneUploadStats();
    const meshUploads = built.meshUploads - before.meshUploads;
    const texUploads = built.textureUploads - before.textureUploads;
    assert(meshUploads >= 12, 'the world uploaded its meshes once: ' + meshUploads);
    assert(texUploads >= 4, 'and its textures: ' + texUploads);
    assert(scene.attached === true && scene.keepAlive === false, 'a fresh scene is attached, not keepAlive');
    assert(__host.sceneLink(c1).layer === true, 'c1 composites the scene');
    const contexts = __host.sceneContextCount();

    // ---- attachTo: both canvases live -------------------------------------
    const c2 = makeCanvas();
    flush();
    assert(scene.attachTo(c2) === scene, 'attachTo returns the scene');
    flush();
    flush();
    let now = __host.sceneUploadStats();
    assert(now.meshUploads === built.meshUploads, 'no mesh re-upload after attachTo: ' + (now.meshUploads - built.meshUploads));
    assert(now.textureUploads === built.textureUploads, 'no texture re-upload after attachTo');
    assert(__host.sceneContextCount() === contexts, 'the same scene context, not a second one');
    assert(__host.sceneLink(c1).graph === false && __host.sceneLink(c1).layer === false, 'c1 no longer shows the scene');
    assert(__host.sceneLink(c2).graph === true && __host.sceneLink(c2).layer === true, 'c2 composites the scene');
    assert(c2.getContext('scene') === scene, "getContext('scene') on c2 answers the moved scene");
    const fresh = c1.getContext('scene');
    assert(fresh && fresh !== scene, "c1 can take a new scene of its own");
    assert(__host.sceneContextCount() === contexts + 1, 'which is a second context');

    // ---- keepAlive: the canvas swap an app does across a rebuild ------------
    scene.keepAlive = true;
    c2.remove();
    flush();
    flush();
    assert(scene.attached === false, 'a keepAlive scene is parked when its canvas leaves the DOM');
    assert(__host.sceneContextCount() === contexts + 1, 'parked, not reclaimed');
    const c3 = makeCanvas();
    flush();
    flush();
    scene.attachTo(c3);
    flush();
    flush();
    now = __host.sceneUploadStats();
    assert(now.meshUploads === built.meshUploads && now.textureUploads === built.textureUploads,
           'no re-upload across a detach + attach to a new canvas');
    assert(scene.attached === true && __host.sceneLink(c3).layer === true, 'c3 composites the parked scene');

    // The scene is still the same live world: a new node uploads once, on c3.
    scene.createMesh({ mesh: 'box', color: 'green' });
    flush();
    scene.captureFrame();
    assert(__host.sceneUploadStats().meshUploads === now.meshUploads + 1, 'a node added after the move uploads once');

    // ---- detach / errors ---------------------------------------------------
    scene.detach();
    flush();
    assert(scene.attached === false && __host.sceneLink(c3).graph === false, 'detach parks the scene');
    let threw = false;
    try { scene.attachTo(document.body); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'attachTo a non-canvas is a TypeError');
    const c2d = makeCanvas();
    c2d.getContext('2d');
    threw = false;
    try { scene.attachTo(c2d); } catch (e) { threw = e instanceof TypeError && /2d/.test(e.message); }
    assert(threw, 'attachTo a 2d canvas is a TypeError');
    threw = false;
    try { scene.attachTo(c1); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'attachTo a canvas showing another scene is a TypeError');
    scene.attachTo(c3);
    flush();
    assert(scene.attached === true && __host.sceneLink(c3).graph === true, 're-attached after detach');

    // Without keepAlive, leaving the DOM still reclaims (the default is unchanged).
    scene.keepAlive = false;
    const n = __host.sceneContextCount();
    c3.remove();
    flush();
    assert(__host.sceneContextCount() === n - 1, 'a scene without keepAlive is reclaimed with its canvas');
    console.log('scene attach: moved across three canvases with ' + meshUploads + ' mesh / ' + texUploads + ' texture uploads, none repeated');
}
