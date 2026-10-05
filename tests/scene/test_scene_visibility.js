// What the scene draws, by the tree:
//   - hiding a node hides its whole subtree: meshes, decals and reflection
//     probes under a hidden group stop drawing (and lighting), and come back
//     when the group shows again;
//   - a scene whose every mesh is culled still composites, so the sky stays
//     when the camera looks away from the geometry.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

function rgbaAt(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]];
}
const fmt = (c) => '(' + c.map((v) => v.toFixed(0)).join(',') + ')';

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const S = 128;

    // --- a hidden group hides its meshes and decals ---------------------------
    {
        const s = freshScene(S);
        const sc = s.scene;
        sc.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 6, 0.01], target: [0, 0, 0] });
        sc.createMesh({ mesh: 'plane', halfW: 5, halfD: 5, color: [0.5, 0.5, 0.5] });
        const group = sc.createNode();
        const box = sc.createMesh({ mesh: Mesh.box(0.6, 0.6, 0.6), x: -2, color: [0, 0, 1] });
        const decal = sc.createDecal({ modulate: [1, 0, 0, 1], size: [1.5, 2, 1.5], x: 2 });
        group.add(box);
        group.add(decal);

        const shown = sc.captureFrame();
        const boxPx = [64 - 40, 64], decalPx = [64 + 40, 64];
        const b0 = rgbaAt(shown, ...boxPx), d0 = rgbaAt(shown, ...decalPx);
        assert(b0[2] > b0[0] + 60, `the box draws blue under a shown group ${fmt(b0)}`);
        assert(d0[0] > d0[2] + 60, `the decal paints red under a shown group ${fmt(d0)}`);

        group.visible = false;
        const hidden = sc.captureFrame();
        const b1 = rgbaAt(hidden, ...boxPx), d1 = rgbaAt(hidden, ...decalPx);
        assert(Math.abs(b1[0] - b1[2]) < 8, `hiding the group hides the box ${fmt(b1)}`);
        assert(Math.abs(d1[0] - d1[2]) < 8, `hiding the group hides the decal ${fmt(d1)}`);

        group.visible = true;
        const back = rgbaAt(sc.captureFrame(), ...boxPx);
        assert(back[2] > back[0] + 60, `showing the group brings the box back ${fmt(back)}`);
        dropScene(s);
    }

    // --- a hidden group's reflection probe stops lighting ---------------------
    {
        const s = freshScene(S);
        const sc = s.scene;
        sc.createLight({ type: 'directional', intensity: 0 });
        sc.setAmbient({ color: [0, 0, 0] });
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 3.5], target: [0, 0, 0] });
        for (const [x, y, z, sx, sy, sz] of [[0, -5, 0, 10.4, 0.2, 10.4], [0, 5, 0, 10.4, 0.2, 10.4],
                                             [-5, 0, 0, 0.2, 10.4, 10.4], [5, 0, 0, 0.2, 10.4, 10.4],
                                             [0, 0, -5, 10.4, 10.4, 0.2], [0, 0, 5, 10.4, 10.4, 0.2]]) {
            sc.createMesh({ mesh: Mesh.box(sx, sy, sz), x, y, z, color: [1, 0.1, 0.1, 1], emissive: 2 });
        }
        sc.createMesh({ mesh: Mesh.sphere(1), color: [1, 1, 1, 1], metallic: 1, roughness: 0.05 });
        const group = sc.createNode();
        const p = sc.createReflectionProbe({ size: 10, resolution: 32, updateMode: 'manual' });
        group.add(p);
        p.capture();
        const lit = rgbaAt(sc.captureFrame(), 64, 64);
        assert(lit[0] > 60, `the probe lights the mirror sphere ${fmt(lit)}`);
        group.visible = false;
        const dark = rgbaAt(sc.captureFrame(), 64, 64);
        assert(dark[0] < 20, `hiding the probe's group takes its lighting away ${fmt(dark)}`);
        dropScene(s);
    }

    // --- every mesh culled: the sky still draws -------------------------------
    {
        const s = freshScene(S);
        const sc = s.scene;
        sc.setToneMap({ mode: 'aces', exposure: 1.0 });
        sc.setAtmosphere({ sunPosition: [0, 0.6, 0.8] });
        sc.setCamera({ fov: 60, near: 0.1, far: 1000, position: [0, 2, 0], target: [0, 4, -5] });
        sc.createMesh({ mesh: 'box', z: 20 });   // behind the camera
        // The composited page, not captureFrame: a scene with nothing to
        // draw drops its layer from the composite.
        s.canvas.style.background = 'black';
        flush();
        const r = s.canvas.getBoundingClientRect();
        const p = getPixel(r.left + 64, r.top + 40);
        const sky = [p.r, p.g, p.b, p.a];
        assert(sky[2] > sky[0] + 20 && sky[2] > 60, `the sky draws with every mesh culled ${fmt(sky)}`);
        dropScene(s);
    }

    console.log('test_scene_visibility: OK');
}
