// Test frustum culling in the 3D scene renderer — forward passes (mesh,
// skinned mesh, instanced mesh, gaussian splat, 3D particles, billboards)
// and the shadow caster pass (per light/cascade tile, never the camera
// frustum). Exercises SceneRenderer::nodeWorldBounds/cameraCulled in
// src/scene/scene_renderer.cpp, the per-tile caster cull in
// scene_renderer_shadow.cpp, SkinnedMeshNode::posedLocalBounds, and the
// scene.setFrustumCulling / scene.cullStats / perf.stats().scene bindings.
//
// Culling is required to be pixel-invisible: every section that toggles it
// compares captures byte-for-byte (tolerance 0 diff pixels at tol 2).

function diffCount(a, b, tol) {
    if (a.width !== b.width || a.height !== b.height) return a.width * a.height;
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        if (Math.abs(a.data[i]     - b.data[i])     > tol ||
            Math.abs(a.data[i + 1] - b.data[i + 1]) > tol ||
            Math.abs(a.data[i + 2] - b.data[i + 2]) > tol ||
            Math.abs(a.data[i + 3] - b.data[i + 3]) > tol) n++;
    }
    return n;
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

function patchBrightness(img, cx, cy, r) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
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
    flush();  // prunes the detached graph so perf.stats() aggregates stay clean
}

const probe = freshScene(64);
if (!probe.scene) {
    console.log('scene context not available (no GPU) — skipping frustum culling test');
} else {
    dropScene(probe);

    // =====================================================================
    // Section 1: mesh culling counts, pixel-identical output, toggle,
    // perf.stats().scene aggregate.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setCamera({ fov: 60, near: 0.1, far: 100,
                        position: [0, 0, 10], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        scn.createMesh({ mesh: 'box', color: 'red' });            // inside
        scn.createMesh({ mesh: 'box', color: 'red', x: -100 });   // far left
        scn.createMesh({ mesh: 'box', color: 'red', x: 100 });    // far right
        scn.createMesh({ mesh: 'box', color: 'red', z: 30 });     // behind camera
        scn.createMesh({ mesh: 'box', color: 'red', z: -200 });   // beyond farZ

        assert(scn.frustumCulling === true, 'frustum culling defaults on');

        const imgOn = scn.captureFrame();
        let st = scn.cullStats();
        assert(st.meshDrawn === 1, `1 mesh drawn with culling on (got ${st.meshDrawn})`);
        assert(st.meshCulled === 4, `4 meshes culled (got ${st.meshCulled})`);
        assert(st.shadowDrawn === 0 && st.shadowCulled === 0,
            'no shadow casters -> zero shadow counters');

        // perf.stats() carries the aggregate for the HUD / headless asserts.
        const ps = perf.stats();
        assert(ps.scene && typeof ps.scene === 'object', 'perf.stats() has a scene block');
        assert(ps.scene.meshDrawn === 1 && ps.scene.meshCulled === 4,
            `perf.stats().scene mirrors cullStats (${ps.scene.meshDrawn}/${ps.scene.meshCulled})`);

        // Toggle off: everything draws, nothing culled, pixels identical.
        scn.setFrustumCulling(false);
        assert(scn.frustumCulling === false, 'setFrustumCulling(false) reads back');
        const imgOff = scn.captureFrame();
        st = scn.cullStats();
        assert(st.meshDrawn === 5, `toggle off draws all 5 (got ${st.meshDrawn})`);
        assert(st.meshCulled === 0, 'toggle off culls nothing');
        const nd = diffCount(imgOn, imgOff, 2);
        assert(nd === 0, `culling is pixel-invisible (diff pixels ${nd})`);

        // Property-setter path re-enables.
        scn.frustumCulling = true;
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.meshDrawn === 1 && st.meshCulled === 4, 'frustumCulling prop setter re-enables');

        dropScene(s);
    }

    // =====================================================================
    // Section 2: instanced meshes (whole-node test), gaussian splats,
    // 3D particles, billboards — one inside + one fully outside each.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setCamera({ fov: 60, near: 0.1, far: 100,
                        position: [0, 0, 10], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        // Instanced: instance rows are row-major 3x4 (rotation | translation).
        const inst = (tx, ty, tz) => [1, 0, 0, tx, 0, 1, 0, ty, 0, 0, 1, tz, 0, 0, 0, 1];
        const boxMesh = Mesh.box();
        scn.createInstancedMesh({ mesh: boxMesh, color: 'red',
            instances: new Float32Array([...inst(0, 0, 0), ...inst(1.5, 0, 0)]) });
        scn.createInstancedMesh({ mesh: boxMesh, color: 'red',
            instances: new Float32Array([...inst(-500, 0, 0), ...inst(-502, 0, 0)]) });

        // Splats: cloud centers are node-local; the off-screen node is moved
        // by its node transform, so the cull must agree with the transformed
        // render position (the pixel-invariance check below would catch a
        // cull/render disagreement either way).
        const mkCloud = (cx) => {
            const N = 3;
            const positions = new Float32Array([cx, 0, 0, cx + 1, 0, 0, cx - 1, 0, 0]);
            const scales = new Float32Array(N * 3).fill(0.1);
            const rotations = new Float32Array(N * 4);
            for (let i = 0; i < N; i++) rotations[i * 4 + 3] = 1;
            const opacities = new Float32Array(N).fill(1);
            const C0 = 0.28209479177387814;
            const sh = new Float32Array(N * 3).fill((1 - 0.5) / C0); // white
            return { positions, scales, rotations, opacities, sh, shDegree: 0 };
        };
        scn.createGaussianSplat({ cloud: mkCloud(0) });
        scn.createGaussianSplat({ cloud: mkCloud(0), x: -1000 });

        // Particles: world-space sims, burst on creation, long lifetime.
        scn.createParticles3D({ position: [0, 0, 0], rate: 0, burst: 20,
            lifetime: 5, seed: 1, size: { start: 0.2, end: 0.2 } });
        scn.createParticles3D({ position: [-1000, 0, 0], rate: 0, burst: 20,
            lifetime: 5, seed: 2, size: { start: 0.2, end: 0.2 } });

        // Billboards (world-anchored shapes).
        scn.createShape({ worldAnchor: [0, 0, 0], width: 1, height: 1, fill: '#4488ff' });
        scn.createShape({ worldAnchor: [-1000, 0, 0], width: 1, height: 1, fill: '#4488ff' });

        advanceTime(50);  // let the particle bursts go live
        const imgOn = scn.captureFrame();
        let st = scn.cullStats();
        assert(st.instancedDrawn === 1 && st.instancedCulled === 1,
            `instanced 1 drawn / 1 culled (got ${st.instancedDrawn}/${st.instancedCulled})`);
        assert(st.splatDrawn === 1 && st.splatCulled === 1,
            `splat 1 drawn / 1 culled (got ${st.splatDrawn}/${st.splatCulled})`);
        assert(st.particlesDrawn === 1 && st.particlesCulled === 1,
            `particles 1 drawn / 1 culled (got ${st.particlesDrawn}/${st.particlesCulled})`);
        assert(st.billboardsDrawn === 1 && st.billboardsCulled === 1,
            `billboards 1 drawn / 1 culled (got ${st.billboardsDrawn}/${st.billboardsCulled})`);

        // Toggle off: same pixels, zero culled everywhere.
        scn.setFrustumCulling(false);
        const imgOff = scn.captureFrame();
        st = scn.cullStats();
        assert(st.instancedCulled === 0 && st.splatCulled === 0 &&
               st.particlesCulled === 0 && st.billboardsCulled === 0,
            'toggle off zeroes all culled counters');
        assert(st.instancedDrawn === 2 && st.splatDrawn === 2 &&
               st.particlesDrawn === 2 && st.billboardsDrawn === 2,
            'toggle off submits both nodes per category');
        const nd = diffCount(imgOn, imgOff, 2);
        assert(nd === 0, `mixed-category culling is pixel-invisible (diff pixels ${nd})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 3: shadow regression — a caster fully OUTSIDE the camera
    // frustum (high above the view) whose shadow lands on ground inside the
    // view. Culling must drop it from the forward pass but keep it in the
    // shadow pass; the shadow must stay pixel-identical.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setCamera({ fov: 45, near: 0.1, far: 50,
                        position: [0, 2, 6], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 4, halfD: 4, color: 'white',
                         castsShadow: false, y: 0 });
        const sun = scn.createLight({ type: 'directional', direction: [0, -1, 0],
                                      color: [1, 1, 1], intensity: 3 });
        sun.castsShadow = true;
        sun.cascadeCount = 1;   // one atlas tile -> exact shadowDrawn counts

        // Caster: a slab 20 units above the ground, far outside the camera
        // frustum, straight-down sun -> its shadow covers the view center.
        const caster = scn.createMesh({ mesh: 'box', halfW: 2, halfH: 0.25, halfD: 2,
                                        y: 20, color: 'red' });

        const withCull = scn.captureFrame();
        let st = scn.cullStats();
        assert(st.meshDrawn === 1 && st.meshCulled === 1,
            `forward pass culls the off-screen caster (${st.meshDrawn}/${st.meshCulled})`);
        assert(st.shadowDrawn === 1 && st.shadowCulled === 0,
            `shadow pass keeps the off-screen caster (${st.shadowDrawn}/${st.shadowCulled})`);

        scn.setFrustumCulling(false);
        const noCull = scn.captureFrame();
        st = scn.cullStats();
        assert(st.meshDrawn === 2 && st.shadowDrawn === 1, 'toggle off draws caster + ground');
        const nd = diffCount(withCull, noCull, 2);
        assert(nd === 0, `shadow from off-screen caster identical with culling (diff pixels ${nd})`);
        scn.setFrustumCulling(true);

        // The shadow is really there: removing the caster's shadow brightens
        // the view center (ground under the slab).
        caster.castsShadow = false;
        const noShadow = scn.captureFrame();
        const shadowB = patchBrightness(withCull, 64, 64, 10);
        const litB = patchBrightness(noShadow, 64, 64, 10);
        assert(litB > shadowB + 20,
            `off-screen caster's shadow visibly darkens the ground (${shadowB} vs ${litB})`);

        dropScene(s);
    }

    // =====================================================================
    // Section 4: skinned no-pop — the palette translates the geometry far
    // from the bind AABB; the camera frames only the POSED position. The
    // posed-bounds cull must keep drawing it (and must cull it once the
    // palette returns it to the origin, proving posed bounds drive the test).
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        // Camera frames x = 20 only; the bind-pose position (origin) is far
        // outside this view.
        scn.setCamera({ fov: 50, near: 0.1, far: 100,
                        position: [20, 0, 5], target: [20, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        scn.createLight({ type: 'directional', direction: [0, 0, -1],
                          color: [1, 1, 1], intensity: 2 });

        // Unit quad at the origin, one full-weight bone.
        const positions = new Float32Array([
            -0.5, -0.5, 0,  0.5, -0.5, 0,  0.5, 0.5, 0,  -0.5, 0.5, 0,
        ]);
        const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]);
        const indices = new Uint32Array([0, 1, 2, 0, 2, 3]);
        const skin = new SkinData({
            boneWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]),
            boneIndices: new Uint32Array(16),
            inverseBindMatrices: new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]),
            boneCount: 1,
        });
        const node = scn.createSkinnedMesh({ positions, normals, indices,
                                             color: 'red', skin });
        assert(node.skinReady === true, 'skin ready');

        // Palette translates the quad to x = 20 (column-major mat4).
        node.setSkinningMatrices(new Float32Array([
            1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  20, 0, 0, 1,
        ]));
        const posed = scn.captureFrame();
        let st = scn.cullStats();
        assert(st.meshDrawn === 1 && st.meshCulled === 0,
            `posed skinned mesh far from bind AABB still draws (${st.meshDrawn}/${st.meshCulled})`);
        assert(patchMaxAlpha(posed, 64, 64, 8) > 128,
            'posed skinned mesh visible at the framed position (no pop)');

        // Identity palette puts the geometry back at the origin — now the
        // POSED bounds are off-screen and the node must cull. This fails if
        // culling ever falls back to a stale/expanded bind box.
        node.setSkinningMatrices(new Float32Array([
            1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1,
        ]));
        const atBind = scn.captureFrame();
        st = scn.cullStats();
        assert(st.meshDrawn === 0 && st.meshCulled === 1,
            `identity palette -> posed bounds off-screen -> culled (${st.meshDrawn}/${st.meshCulled})`);
        assert(patchMaxAlpha(atBind, 64, 64, 8) === 0, 'quad gone once posed back at origin');

        dropScene(s);
    }

    console.log('frustum culling tests passed');
}
