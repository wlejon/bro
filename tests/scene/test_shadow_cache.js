// Static shadow-tile cache (scene_renderer_shadow.cpp) — atlas tiles whose
// light projection and overlapping caster set are unchanged are reused
// instead of re-rendered every frame.
//
// Contract under test:
//   - Caching is pixel-INVISIBLE: captures are byte-identical (tolerance 0)
//     frame-over-frame on a static scene, and between cache on and off.
//   - Stats: perf.stats().scene / scene.cullStats() report
//     shadowTilesTotal / shadowTilesRendered / shadowTilesCached per frame.
//   - Partial invalidation: a moving caster re-renders only the tiles whose
//     light frustum it overlaps; other lights' tiles stay cached.
//   - Light changes (move, cone angle) invalidate that light's tiles only.
//   - Camera movement invalidates directional cascades (the CSM fit follows
//     the camera) but NOT spot/point tiles (camera-independent).
//   - Skinned casters are permanently dynamic: their tiles re-render every
//     frame even when nothing changed.

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
    console.log('scene context not available (no GPU) — skipping shadow cache test');
} else {
    dropScene(probe);

    // =====================================================================
    // Section 1: static scene, directional + spot + point — everything
    // caches after frame 1, captures stay byte-identical, and camera motion
    // invalidates exactly the directional cascades.
    // =====================================================================
    {
        const s = freshScene(160);
        const scn = s.scene;
        scn.setCamera({ fov: 50, near: 0.1, far: 60,
                        position: [0, 6, 12], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        assert(scn.shadowCache === true, 'shadow cache defaults on');

        scn.createMesh({ mesh: 'plane', halfW: 10, halfD: 10, color: 'white',
                         castsShadow: false, y: 0 });
        scn.createMesh({ mesh: 'box', color: 'red',   x: -3, y: 1 });
        scn.createMesh({ mesh: 'box', color: 'green', x:  0, y: 1 });
        scn.createMesh({ mesh: 'box', color: 'blue',  x:  3, y: 1 });

        const sun = scn.createLight({ type: 'directional',
                                      direction: [-0.3, -1, -0.2], intensity: 2 });
        sun.castsShadow = true;
        sun.cascadeCount = 2;
        const spot = scn.createLight({ type: 'spot', position: [-3, 5, 0],
                                       direction: [0, -1, 0], intensity: 30 });
        spot.castsShadow = true;
        const lamp = scn.createLight({ type: 'point', position: [3, 4, 0],
                                       range: 12, intensity: 25 });
        lamp.castsShadow = true;
        // 2 cascades + 1 spot + 6 point faces = 9 atlas tiles.

        const f1 = scn.captureFrame();
        let st = scn.cullStats();
        assert(st.shadowTilesTotal === 9,
            `9 tiles allocated (got ${st.shadowTilesTotal})`);
        assert(st.shadowTilesRendered === 9 && st.shadowTilesCached === 0,
            `cold frame renders all tiles (${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        assert(st.shadowDrawn > 0, 'cold frame submits casters');

        const f2 = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 0 && st.shadowTilesCached === 9,
            `static frame 2 reuses all tiles (${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        assert(st.shadowDrawn === 0 && st.shadowCulled === 0,
            'cached tiles submit no casters at all');

        // perf.stats().scene carries the same counters.
        const ps = perf.stats();
        assert(ps.scene.shadowTilesTotal === 9 && ps.scene.shadowTilesCached === 9,
            `perf.stats().scene mirrors tile counters (${ps.scene.shadowTilesTotal}/${ps.scene.shadowTilesCached})`);

        advanceTime(50 * 16);  // 50 virtual frames later — still nothing moved
        const f3 = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 9, 'still fully cached after advanceTime');

        assert(diffCount(f1, f2, 0) === 0, 'frames 1 and 2 byte-identical');
        assert(diffCount(f1, f3, 0) === 0, 'frames 1 and N+50 byte-identical');

        // Camera moves -> the CSM fit follows it, so exactly the 2 cascades
        // re-render; spot/point tiles are camera-independent and stay cached.
        scn.setCamera({ fov: 50, near: 0.1, far: 60,
                        position: [0.5, 6, 12], target: [0, 0, 0] });
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 2 && st.shadowTilesCached === 7,
            `camera move re-renders only the cascades (${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 9, 'camera still again -> fully cached');

        // ------------------------------------------------------------------
        // Section 2 (same scene): cache off vs on is pixel-identical.
        // ------------------------------------------------------------------
        scn.setCamera({ fov: 50, near: 0.1, far: 60,
                        position: [0, 6, 12], target: [0, 0, 0] });
        scn.captureFrame();  // settle back to the section-1 view
        const cachedImg = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 9, 'baseline fully cached');

        scn.setShadowCache({ enabled: false });
        assert(scn.shadowCache === false, 'setShadowCache(false) reads back');
        const uncachedImg = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 9 && st.shadowTilesCached === 0,
            'cache off re-renders every tile every frame');
        assert(st.shadowDrawn > 0, 'cache off submits casters again');
        assert(diffCount(cachedImg, uncachedImg, 0) === 0,
            'cache on vs off byte-identical');

        scn.shadowCache = true;  // property-setter path
        assert(scn.shadowCache === true, 'shadowCache prop setter re-enables');
        scn.captureFrame();      // re-seed (toggle invalidates)
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 9, 'toggle invalidates: first frame re-renders');
        const reCachedImg = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 9, 're-enabled cache fully reuses');
        assert(diffCount(cachedImg, reCachedImg, 0) === 0,
            're-enabled cache byte-identical');

        dropScene(s);
    }

    // =====================================================================
    // Section 3: partial invalidation — a caster moving inside spot A's
    // frustum re-renders only A's tile; spot B's tile stays cached. Then
    // light-property changes invalidate exactly that light's tile.
    // =====================================================================
    {
        const s = freshScene(160);
        const scn = s.scene;
        scn.setCamera({ fov: 55, near: 0.1, far: 60,
                        position: [0, 8, 14], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 8, color: 'white',
                         castsShadow: false, y: 0 });
        const mover = scn.createMesh({ mesh: 'box', color: 'red',
                                       x: -5, y: 1.2 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 5, y: 1.2 });

        const spotA = scn.createLight({ type: 'spot', position: [-5, 5, 0],
                                        direction: [0, -1, 0], intensity: 40 });
        spotA.castsShadow = true;
        spotA.range = 6;
        const spotB = scn.createLight({ type: 'spot', position: [5, 5, 0],
                                        direction: [0, -1, 0], intensity: 40 });
        spotB.castsShadow = true;
        spotB.range = 6;

        const before = scn.captureFrame();  // cold
        scn.captureFrame();                 // warm
        let st = scn.cullStats();
        assert(st.shadowTilesTotal === 2 && st.shadowTilesCached === 2,
            `two spot tiles, both cached when static (${st.shadowTilesCached})`);

        // Animate the caster through spot A's frustum: every frame it moves,
        // only A's tile re-renders, and the shadow visibly tracks it.
        let duringImg = null;
        for (let i = 0; i < 4; ++i) {
            mover.x += 0.25;
            duringImg = scn.captureFrame();
            st = scn.cullStats();
            assert(st.shadowTilesRendered === 1 && st.shadowTilesCached === 1,
                `moving caster re-renders only spot A's tile (frame ${i}: ` +
                `${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        }
        assert(diffCount(before, duringImg, 2) > 0,
            'shadow visibly updates while the caster moves');

        // Caster stops -> back to fully cached.
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 2, 'stopped caster -> fully cached again');

        // Light moved -> its projection changed -> only its tile re-renders.
        const preLightMove = scn.captureFrame();
        spotB.x += 0.8;
        const postLightMove = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 1 && st.shadowTilesCached === 1,
            `moving spot B re-renders only its tile (${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        assert(diffCount(preLightMove, postLightMove, 2) > 0,
            'moving the light visibly changes the frame');

        // Cone angle (shadow FOV) change -> same story.
        scn.captureFrame();
        spotB.outerAngle = 0.7;
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesRendered === 1 && st.shadowTilesCached === 1,
            `spot FOV change re-renders only its tile (${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowTilesCached === 2, 'light untouched again -> fully cached');

        dropScene(s);
    }

    // =====================================================================
    // Section 4: skinned casters are permanently dynamic — the tile they
    // overlap re-renders every frame even with an unchanged palette, while
    // an unaffected light's tile stays cached.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setCamera({ fov: 55, near: 0.1, far: 60,
                        position: [0, 6, 12], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 8, color: 'white',
                         castsShadow: false, y: 0 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 5, y: 1.2 });

        const spotA = scn.createLight({ type: 'spot', position: [-5, 5, 0],
                                        direction: [0, -1, 0], intensity: 40 });
        spotA.castsShadow = true;
        spotA.range = 6;
        const spotB = scn.createLight({ type: 'spot', position: [5, 5, 0],
                                        direction: [0, -1, 0], intensity: 40 });
        spotB.castsShadow = true;
        spotB.range = 6;

        // Skinned quad under spot A (bind pose at the node origin).
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
        const skinned = scn.createSkinnedMesh({ positions, normals, indices,
                                                color: 'red', skin });
        skinned.x = -5;
        skinned.y = 1.5;
        assert(skinned.skinReady === true, 'skin ready');

        scn.captureFrame();  // cold
        for (let i = 0; i < 3; ++i) {
            scn.captureFrame();
            const st = scn.cullStats();
            assert(st.shadowTilesTotal === 2, 'two spot tiles');
            assert(st.shadowTilesRendered === 1 && st.shadowTilesCached === 1,
                `skinned caster keeps its tile dynamic (frame ${i}: ` +
                `${st.shadowTilesRendered}/${st.shadowTilesCached})`);
        }

        dropScene(s);
    }

    console.log('shadow cache tests passed');
}
