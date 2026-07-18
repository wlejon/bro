// Mesh LOD chains (meshNode.setLodMeshes) + per-node visibility ranges
// (node.visibilityRange) — Godot visibility_range / mesh-LOD analogs.
// Verifies:
//   - the renderer picks the LOD level by camera distance (geometry-size
//     pixel probes prove WHICH mesh drew; node.lodLevel introspects)
//   - beyond the last maxDist the coarsest level keeps drawing
//   - visibility range gates rendering by camera distance without touching
//     node.visible (independence both ways), with hysteresis via `margin`
//     and a hard switch at margin 0
// Exercises SceneGraph::updateVisibilityGates (scene_graph.cpp),
// MeshNode::setLodMeshes/selectLodByDistance (mesh_node.cpp), and
// SceneNode::updateRangeGate/renderVisible (scene_node.h).

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
    console.log('scene context not available (no GPU) — skipping LOD/visibility test');
} else {
    dropScene(probe);

    // =====================================================================
    // Section 1: LOD chain — big box (half 2) up to 15 units, small box
    // (half 1) beyond. An offset pixel only the big box covers proves which
    // level rendered; lodLevel introspects the same selection.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 2 });
        scn.setCamera({ fov: 60, near: 0.1, far: 200,
                        position: [0, 0, 10], target: [0, 0, 0] });

        const big = Mesh.box(2, 2, 2);
        const small = Mesh.box(1, 1, 1);
        const node = scn.createMesh({ mesh: 'box', color: 'red' });
        node.setLodMeshes([
            { mesh: big, maxDist: 15 },
            { mesh: small, maxDist: 40 },
        ]);
        assert(node.lodCount === 2, 'lodCount reads back');

        // d = 10 < 15 -> level 0 (big). Front face at z=2 (8 from the
        // camera): the big box spans ~±28 px, the small ~±14 px — probe at
        // +20 px, covered only by the big box.
        let img = scn.captureFrame();
        assert(node.lodLevel === 0, `near camera selects level 0 (got ${node.lodLevel})`);
        assert(patchMaxAlpha(img, 64 + 20, 64, 2) > 128,
            'big-box level covers the offset probe');

        // d = 30 -> level 1 (small): offset probe empty, center still drawn.
        scn.setCamera({ fov: 60, near: 0.1, far: 200,
                        position: [0, 0, 30], target: [0, 0, 0] });
        img = scn.captureFrame();
        assert(node.lodLevel === 1, `far camera selects level 1 (got ${node.lodLevel})`);
        assert(patchMaxAlpha(img, 64 + 20, 64, 2) === 0,
            'small-box level leaves the offset probe empty');
        assert(patchMaxAlpha(img, 64, 64, 3) > 128,
            'small-box level still draws at the center');

        // Beyond the last maxDist (d = 60 > 40) the coarsest level persists.
        scn.setCamera({ fov: 60, near: 0.1, far: 200,
                        position: [0, 0, 60], target: [0, 0, 0] });
        img = scn.captureFrame();
        assert(node.lodLevel === 1, 'beyond the last maxDist the coarsest level stays');
        assert(patchMaxAlpha(img, 64, 64, 3) > 0,
            'coarsest level keeps drawing beyond the last maxDist');

        // Back near: switches back to level 0.
        scn.setCamera({ fov: 60, near: 0.1, far: 200,
                        position: [0, 0, 10], target: [0, 0, 0] });
        img = scn.captureFrame();
        assert(node.lodLevel === 0, 'moving back re-selects level 0');
        assert(patchMaxAlpha(img, 64 + 20, 64, 2) > 128, 'big box again');

        // Clearing the chain returns rendering to the base mesh (half 0.5:
        // covers the center but not even the small box's +12 px probe).
        node.setLodMeshes([]);
        assert(node.lodCount === 0, 'empty array clears the chain');
        img = scn.captureFrame();
        assert(patchMaxAlpha(img, 64, 64, 2) > 128 &&
               patchMaxAlpha(img, 64 + 12, 64, 2) === 0,
            'cleared chain renders the base mesh');

        dropScene(s);
    }

    // =====================================================================
    // Section 2: visibility range + hysteresis. Box half 1 at the origin;
    // camera distance = its z position. Range [0, 20) with margin 2.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 2 });
        const camAt = (d) => scn.setCamera({ fov: 60, near: 0.1, far: 200,
                                             position: [0, 0, d], target: [0, 0, 0] });
        const visible = () => patchMaxAlpha(scn.captureFrame(), 64, 64, 3) > 0;

        // Keeper: an always-visible plane well below the center probe. When
        // the gated box is hidden the scene must still have 3D content, or
        // the tonemap FBO (and thus the readback) would keep the previous
        // frame's pixels instead of re-rendering an empty view.
        scn.createMesh({ mesh: 'plane', halfW: 3, halfD: 3, y: -3,
                         color: 'white', castsShadow: false });

        const box = scn.createMesh({ mesh: 'box', halfW: 1, halfH: 1, halfD: 1,
                                     color: 'red' });
        camAt(10);
        assert(visible(), 'baseline: box visible with no range');

        box.visibilityRange = { begin: 0, end: 20, margin: 2 };
        const vr = box.visibilityRange;
        assert(vr && vr.begin === 0 && vr.end === 20 && vr.margin === 2,
            'visibilityRange reads back');

        assert(visible(), 'd=10 inside [0,20) -> visible');
        assert(box.visible === true, 'gate never mutates node.visible');

        // Hysteresis at the far edge: shown stays shown until d >= end+margin.
        camAt(21);
        assert(visible(), 'd=21: shown state persists inside the margin (no pop)');
        camAt(23);
        assert(!visible(), 'd=23 >= end+margin -> hidden');
        camAt(21);
        assert(!visible(), 'd=21: hidden state persists inside the margin (no pop)');
        camAt(17);
        assert(visible(), 'd=17 < end-margin -> shown again');
        assert(box.visible === true, 'node.visible still untouched');

        // Independence: the user flag gates on top of an open range gate.
        box.visible = false;
        assert(!visible(), 'visible=false hides regardless of the open gate');
        box.visible = true;
        assert(visible(), 'visible=true restores (gate still open)');

        // margin 0 = hard switch at the boundary.
        box.visibilityRange = { begin: 0, end: 20 };
        camAt(20.5);
        assert(!visible(), 'margin 0: d=20.5 hidden');
        camAt(19.5);
        assert(visible(), 'margin 0: d=19.5 visible (hard switch)');

        // begin edge: node only appears beyond `begin`.
        box.visibilityRange = { begin: 30, end: 1e30 };
        camAt(19.5);
        assert(!visible(), 'd < begin -> hidden');
        camAt(35);
        assert(visible(), 'd >= begin -> visible');

        // Clearing restores unconditional rendering.
        box.visibilityRange = null;
        assert(box.visibilityRange === null, 'null clears the range');
        camAt(10);
        assert(visible(), 'cleared range renders at any distance');

        dropScene(s);
    }

    console.log('LOD + visibility range tests passed');
}
