// Cached shadow pages (world/shadow_pages.h): a page's depth is kept until a caster or its light changes, so a
// still scene draws no shadow casters at all once its pages are in.
//
// Contract under test:
//   - Caching is pixel-invisible: once the pages a view needs are drawn,
//     captures are byte-identical frame over frame.
//   - Stats: scene.cullStats() / perf.stats().scene report the pages needed,
//     resident and drawn each frame.
//   - A camera pan keeps the pages drawn (the clipmap is fixed in light
//     space): it draws only the pages that come into view.
//   - A caster's change redraws the pages its bounds touch, the frame it
//     changes; a light's move redraws its own.
//   - What moves every frame (a skinned caster) is drawn into overlays each
//     frame over the cached depth, never by redrawing pages.

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

// Frames until no page is wanted and none drawn: the pages the view needs
// are in. Returns how many it took.
function settle(scn) {
    for (let i = 1; i <= 10; ++i) {
        scn.captureFrame();
        const st = scn.cullStats();
        // (The pixels' requests come back a frame or two late.)
        if (i > 3 && st.shadowPagesWanted === 0 && st.shadowPagesDrawn === 0) return i;
    }
    return -1;
}

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);

    // =====================================================================
    // Section 1: a still scene with a sun, a spot and a point light caches
    // every page; captures stay byte-identical; a pan draws only new pages.
    // =====================================================================
    {
        const s = freshScene(160);
        const scn = s.scene;
        scn.setCamera({ fov: 50, near: 0.1, far: 60, position: [0, 6, 12], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 10, halfD: 10, color: 'white', castsShadow: false, y: 0 });
        scn.createMesh({ mesh: 'box', color: 'red',   x: -3, y: 1 });
        scn.createMesh({ mesh: 'box', color: 'green', x:  0, y: 1 });
        scn.createMesh({ mesh: 'box', color: 'blue',  x:  3, y: 1 });

        const sun = scn.createLight({ type: 'directional', direction: [-0.3, -1, -0.2], intensity: 2 });
        sun.castsShadow = true;
        const spot = scn.createLight({ type: 'spot', position: [-3, 5, 0], direction: [0, -1, 0], intensity: 30 });
        spot.castsShadow = true;
        const lamp = scn.createLight({ type: 'point', position: [3, 4, 0], range: 12, intensity: 25 });
        lamp.castsShadow = true;

        scn.captureFrame();
        let st = scn.cullStats();
        assert(st.shadowPagesDrawn > 0 && st.shadowDrawn > 0,
            `the first frame draws pages (${st.shadowPagesDrawn}) and casters (${st.shadowDrawn})`);
        const frames = settle(scn);
        assert(frames > 0, `the pages settle (${frames} frames)`);

        const f1 = scn.captureFrame();
        st = scn.cullStats();
        assert(st.shadowPagesDrawn === 0 && st.shadowDrawn === 0 && st.shadowViews === 0,
            `a still frame draws nothing (${st.shadowPagesDrawn} pages, ${st.shadowDrawn} casters)`);
        assert(st.shadowPagesNeeded > 0 && st.shadowPagesResident === st.shadowPagesNeeded,
            `every page needed is resident (${st.shadowPagesResident} of ${st.shadowPagesNeeded})`);

        const ps = perf.stats();
        assert(ps.scene.shadowPagesNeeded === st.shadowPagesNeeded && ps.scene.shadowPagesDrawn === 0 &&
               ps.scene.shadowPagesResident === st.shadowPagesResident,
            `perf.stats().scene mirrors the page counters (${ps.scene.shadowPagesNeeded}/${ps.scene.shadowPagesResident})`);

        advanceTime(50 * 16);  // 50 virtual frames later, still nothing moved
        const f2 = scn.captureFrame();
        assert(scn.cullStats().shadowPagesDrawn === 0, 'still cached after advanceTime');
        assert(diffCount(f1, f2, 0) === 0, 'frames before and after byte-identical');

        // A small pan: the pages already drawn stay; only those coming into
        // view are drawn.
        const resident = st.shadowPagesResident;
        let drawn = 0;
        for (let i = 1; i <= 4; ++i) {
            scn.setCamera({ fov: 50, near: 0.1, far: 60, position: [0.25 * i, 6, 12], target: [0.25 * i, 0, 0] });
            scn.captureFrame();
            drawn += scn.cullStats().shadowPagesDrawn;
        }
        assert(drawn < resident, `a pan draws only the pages it brings in (${drawn} drawn, ${resident} resident)`);
        assert(settle(scn) > 0, 'and settles again');
        dropScene(s);
    }

    // =====================================================================
    // Section 2: a caster's move redraws pages the frame it moves, and the
    // shadow follows it; a light's move redraws its pages.
    // =====================================================================
    {
        const s = freshScene(160);
        const scn = s.scene;
        scn.setCamera({ fov: 55, near: 0.1, far: 60, position: [0, 8, 14], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 8, color: 'white', castsShadow: false, y: 0 });
        const mover = scn.createMesh({ mesh: 'box', color: 'red', x: -5, y: 1.2 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 5, y: 1.2 });

        const spotA = scn.createLight({ type: 'spot', position: [-5, 5, 0], direction: [0, -1, 0], intensity: 40 });
        spotA.castsShadow = true;
        spotA.range = 6;
        const spotB = scn.createLight({ type: 'spot', position: [5, 5, 0], direction: [0, -1, 0], intensity: 40 });
        spotB.castsShadow = true;
        spotB.range = 6;

        assert(settle(scn) > 0, 'two spots settle');
        const before = scn.captureFrame();

        // A caster that moves is drawn every frame (an overlay) while it
        // moves, and its shadow tracks it.
        let during = null;
        for (let i = 0; i < 4; ++i) {
            mover.x += 0.25;
            during = scn.captureFrame();
            const st = scn.cullStats();
            assert(st.shadowPagesDynamic > 0, `the moving caster is drawn this frame (frame ${i}: ${st.shadowPagesDynamic})`);
        }
        assert(diffCount(before, during, 2) > 0, 'the shadow follows the caster');
        // Come to rest (a few frames without a change), it is back in the
        // cached depth.
        for (let i = 0; i < 10; ++i) scn.captureFrame();
        assert(settle(scn) > 0, 'the caster at rest settles');
        const rest = scn.captureFrame();
        assert(scn.cullStats().shadowPagesDynamic === 0, 'a caster at rest draws no overlay');
        assert(diffCount(during, rest, 2) < 160 * 160 / 400, 'at rest it shadows as it did moving');

        // The light moves: its pages are redrawn.
        spotB.x += 0.8;
        const moved = scn.captureFrame();
        assert(scn.cullStats().shadowPagesDrawn > 0, 'a moved light redraws its pages');
        assert(diffCount(rest, moved, 2) > 0, 'moving the light visibly changes the frame');
        assert(settle(scn) > 0, 'the light at rest settles');
        dropScene(s);
    }

    // =====================================================================
    // Section 3: a skinned caster moves every frame: drawn into overlays,
    // the cached pages untouched.
    // =====================================================================
    {
        const s = freshScene(128);
        const scn = s.scene;
        scn.setCamera({ fov: 55, near: 0.1, far: 60, position: [0, 6, 12], target: [0, 0, 0] });
        scn.setToneMap({ mode: 'linear', gamma: 1.0 });

        scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 8, color: 'white', castsShadow: false, y: 0 });
        scn.createMesh({ mesh: 'box', color: 'blue', x: 5, y: 1.2 });
        const sun = scn.createLight({ type: 'directional', direction: [-0.3, -1, -0.2], intensity: 2 });
        sun.castsShadow = true;

        const positions = new Float32Array([-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0]);
        const normals = new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]);
        const indices = new Uint32Array([0, 1, 2, 0, 2, 3]);
        const skin = new SkinData({
            boneWeights: new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]),
            boneIndices: new Uint32Array(16),
            inverseBindMatrices: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
            boneCount: 1,
        });
        const skinned = scn.createSkinnedMesh({ positions, normals, indices, color: 'red', skin });
        skinned.x = -5;
        skinned.y = 1.5;
        assert(skinned.skinReady === true, 'skin ready');

        for (let i = 0; i < 6; ++i) scn.captureFrame();
        for (let i = 0; i < 3; ++i) {
            scn.captureFrame();
            const st = scn.cullStats();
            assert(st.shadowPagesDynamic > 0 && st.shadowPagesDrawn === 0,
                `a skinned caster is an overlay each frame, no page redrawn (frame ${i}: ` +
                `${st.shadowPagesDynamic} overlays, ${st.shadowPagesDrawn} pages)`);
        }
        dropScene(s);
    }

    console.log('shadow cache tests passed');
}
