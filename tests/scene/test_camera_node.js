// Camera nodes (Godot Camera3D analog) — scene.createCamera / setActiveCamera
// / scene.activeCamera. The node's WORLD transform drives the view (camera
// looks down local -Z, +Y up), so cameras parent under moving nodes and
// inherit their motion; projection params (fov/near/far/aspect/size) live on
// the node. Verifies:
//   - switching the active camera changes the rendered view (pixel probes on
//     two differently-colored meshes, via the readTonemap readback path)
//   - a camera parented under a tweened rig inherits the motion each tick
//   - precedence: imperative setCamera() deactivates the camera node (last
//     camera call wins); setActiveCamera(null) keeps the last derived view
//   - projection params read back and are live-editable (fov zoom)
// Exercises SceneGraph::setActiveCamera/applyActiveCamera (scene_graph.cpp)
// and CameraNode (camera_node.h).

function patchChannelAvg(img, cx, cy, r, ch) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            sum += img.data[(y * img.width + x) * 4 + ch];
            n++;
        }
    }
    return n ? sum / n : 0;
}

function patchMaxAlpha(img, cx, cy, r) {
    let m = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            m = Math.max(m, img.data[(y * img.width + x) * 4 + 3]);
        }
    }
    return m;
}

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    return { canvas: cv, scene: cv.getContext('scene') };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping camera node test');
} else {
    dropScene(probe);

    // =====================================================================
    // Section 1: active-camera switch changes the rendered view.
    // Red box at x=0, blue box at x=100; one camera aimed at each.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 2 });

        scn.createMesh({ mesh: 'box', color: 'red', x: 0 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 100 });

        const camA = scn.createCamera({ position: [0, 0, 5], lookAt: [0, 0, 0],
                                        active: true, name: 'camA' });
        const camB = scn.createCamera({ position: [100, 0, 5], lookAt: [100, 0, 0],
                                        name: 'camB' });

        assert(camA.type === 'camera', 'camera node type reads "camera"');
        assert(scn.activeCamera && scn.activeCamera.id === camA.id,
            'createCamera({active:true}) activates');
        assert(Math.abs(camA.fov - 60) < 0.01, `fov defaults to 60 deg (got ${camA.fov})`);
        assert(Math.abs(camA.near - 0.1) < 1e-6 && Math.abs(camA.far - 1000) < 1e-3,
            'near/far defaults read back');
        assert(camA.projection === 'perspective', 'projection defaults to perspective');

        let img = scn.captureFrame();
        let r = patchChannelAvg(img, 64, 64, 4, 0);
        let b = patchChannelAvg(img, 64, 64, 4, 2);
        assert(r > 150 && b < 60,
            `camera A sees the red box (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        // Switch — the readback (captureFrame -> readTonemapPixelsRGBA) must
        // reflect the new active camera immediately.
        scn.setActiveCamera(camB);
        assert(scn.activeCamera.id === camB.id, 'setActiveCamera switches');
        img = scn.captureFrame();
        r = patchChannelAvg(img, 64, 64, 4, 0);
        b = patchChannelAvg(img, 64, 64, 4, 2);
        assert(b > 150 && r < 60,
            `camera B sees the blue box (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        // setActiveCamera(null): deactivates but KEEPS the last derived view.
        scn.setActiveCamera(null);
        assert(scn.activeCamera === null, 'setActiveCamera(null) deactivates');
        img = scn.captureFrame();
        b = patchChannelAvg(img, 64, 64, 4, 2);
        assert(b > 150, 'deactivating keeps the last derived view (still blue)');

        // Precedence: imperative setCamera() wins and deactivates.
        scn.setActiveCamera(camB);
        scn.setCamera({ fov: 60, near: 0.1, far: 100,
                        position: [0, 0, 5], target: [0, 0, 0] });
        assert(scn.activeCamera === null,
            'imperative setCamera() deactivates the camera node');
        img = scn.captureFrame();
        r = patchChannelAvg(img, 64, 64, 4, 0);
        assert(r > 150, 'imperative view renders (red box)');

        // And back: setActiveCamera overrides the imperative view.
        scn.setActiveCamera(camB);
        img = scn.captureFrame();
        b = patchChannelAvg(img, 64, 64, 4, 2);
        assert(b > 150, 're-activating the camera node overrides the imperative view');

        // Projection params are live: zooming out (wider fov) shrinks the
        // box's screen coverage. Probe an offset pixel the box covers at
        // fov 25 but not at fov 90 (box half 0.5, front face 4.5 from cam).
        scn.setActiveCamera(camA);
        camA.fov = 25;
        img = scn.captureFrame();
        const alphaNarrow = patchMaxAlpha(img, 64 + 24, 64, 2);
        camA.fov = 90;
        img = scn.captureFrame();
        const alphaWide = patchMaxAlpha(img, 64 + 24, 64, 2);
        assert(Math.abs(camA.fov - 90) < 0.01, 'fov round-trips');
        assert(alphaNarrow > 128 && alphaWide === 0,
            `fov edit changes the projection (narrow=${alphaNarrow} wide=${alphaWide})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 2: a camera parented under a rig inherits tweened motion —
    // the view follows on the same tick the tween runs, no per-frame JS.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 2 });

        scn.createMesh({ mesh: 'box', color: 'red', x: 0 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 100 });

        const rig = scn.createNode('rig');
        const cam = scn.createCamera({ position: [0, 0, 5] });  // local -Z view
        rig.add(cam);
        scn.setActiveCamera(cam);

        let img = scn.captureFrame();
        let r = patchChannelAvg(img, 64, 64, 4, 0);
        assert(r > 150, 'rig at origin: parented camera sees the red box');

        const tw = scn.createTween()
            .to(rig, { position: [100, 0, 0] }, 0.5, { easing: 'linear' })
            .start();

        // Mid-tween (~x=50): neither box is in view — empty frame center.
        advanceTime(250);
        img = scn.captureFrame();
        assert(patchMaxAlpha(img, 64, 64, 4) === 0,
            'mid-tween the parented camera is between the boxes (empty center)');

        // Tween done (~x=100): the camera inherited the rig's full motion.
        advanceTime(400);
        assert(tw.isFinished, 'tween finished');
        img = scn.captureFrame();
        const b = patchChannelAvg(img, 64, 64, 4, 2);
        r = patchChannelAvg(img, 64, 64, 4, 0);
        assert(b > 150 && r < 60,
            `after the tween the parented camera sees the blue box (r=${r.toFixed(0)} b=${b.toFixed(0)})`);

        // Destroying the active camera: view freezes, activeCamera reads null.
        scn.destroyNode(cam);
        assert(scn.activeCamera === null, 'destroyed active camera reads back null');
        img = scn.captureFrame();
        assert(patchChannelAvg(img, 64, 64, 4, 2) > 150,
            'last derived view is kept after the active camera is destroyed');

        dropScene(s);
    }

    console.log('camera node tests passed');
}
