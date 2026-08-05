// A scene context built from C++ must be the same thing canvas.getContext('scene')
// builds — same registration, same rendering, same compositing.
//
// This is the test for Engine::createSceneContext(dom::Element*) and for the
// host's route to an Engine*, bro::engine::engineForContext(JSContext*). It is
// driven through `__host`, a deliberately tiny host application installed by
// bro-headless via HeadlessHooks::installHostBindings (src/headless/main.cpp) —
// the same hook planet-bro and ffmpeg-bro use, and the same shape broc's
// generated C++ has. Nothing here reaches into the engine any other way.
//
// Why it matters: a SceneGraph built with make_unique outside createSceneContext
// is not in Engine::sceneGraphs_, so the frame loop never renders it, and has no
// FBO-texture callback, so the compositor never sees it. Both failures look
// exactly like "the renderer is broken", and the workaround for them
// (glBlitFramebuffer straight to framebuffer 0) was reverted in f285ae6d
// because it ignored layout, scroll, z-order and clipping. This test fails if
// either registration goes missing.
//
// Also covers dom::Node::appendChild invalidating layout on its own — a host
// has no js_element_appendChild to compensate for it, so a canvas appended from
// C++ must enter layout with no follow-up call.

const SIZE = 96;

function patchAvg(img, cx, cy, r, ch) {
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

// Max absolute per-channel difference over a grid of samples. Both scenes are
// the same size with the same content on the same GPU, so this should be 0;
// the tolerance only exists so a driver's last-bit dither can't fail the run.
function maxDiff(a, b) {
    if (a.width !== b.width || a.height !== b.height) return 255;
    let m = 0;
    for (let y = 0; y < a.height; y += 3) {
        for (let x = 0; x < a.width; x += 3) {
            const i = (y * a.width + x) * 4;
            for (let c = 0; c < 4; c++) {
                m = Math.max(m, Math.abs(a.data[i + c] - b.data[i + c]));
            }
        }
    }
    return m;
}

// The identical scene, built through whichever wrapper it is handed.
function buildScene(scn) {
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0, 0, -1],
                      color: [1, 1, 1], intensity: 2 });
    scn.createMesh({ mesh: 'box', color: 'red', x: 0, y: 0, z: 0 });
    scn.setCamera({ fov: 60, near: 0.1, far: 100,
                    position: [0, 0, 5], target: [0, 0, 0] });
}

function centerOf(el) {
    const r = el.getBoundingClientRect();
    return { x: Math.round(r.x + r.width / 2), y: Math.round(r.y + r.height / 2) };
}

// =========================================================================
// Section 0: the host can reach the Engine at all.
// =========================================================================
assert(typeof __host === 'object' && __host,
    '__host bindings installed (EngineConfig::installHostBindings ran)');
assert(__host.engineResolvedAtInstall() === true,
    'engineForContext() answers INSIDE the host installer, not just later — ' +
    'the engine back-pointer must be registered before installCoreBindings');

// =========================================================================
// Section 1: the JS path, as the baseline.
// =========================================================================
const jsCanvas = document.createElement('canvas');
jsCanvas.setAttribute('width', String(SIZE));
jsCanvas.setAttribute('height', String(SIZE));
jsCanvas.style.width = SIZE + 'px';
jsCanvas.style.height = SIZE + 'px';
jsCanvas.style.display = 'block';
document.body.appendChild(jsCanvas);
flush();

const jsScene = jsCanvas.getContext('scene');

if (!jsScene) {
    // Same skip the other scene tests take: no GL context, so getContext('scene')
    // is null by design and createSceneContext must agree.
    const probe = __host.createCanvas('probe-nogpu', SIZE, SIZE);
    assert(__host.sceneContext(probe) === null,
        'with no GPU the C++ path returns null too, exactly as getContext does');
    console.log('scene context not available (no GPU) — skipping host scene context test');
} else {

    const countAfterJs = __host.sceneContextCount();
    assert(countAfterJs >= 1, 'the JS scene context registered a scene graph');

    // =====================================================================
    // Section 2: the C++ path — createElement + appendChild + createSceneContext,
    // all from C++, nothing routed through a JS DOM binding.
    // =====================================================================
    const hostCanvas = __host.createCanvas('host-canvas', SIZE, SIZE);
    assert(hostCanvas, '__host.createCanvas returned an element');
    assert(document.getElementById('host-canvas') === hostCanvas,
        'the C++-created canvas is in the document and findable from JS');

    // dom::Node::appendChild must invalidate layout by itself. If it does not,
    // the element never gets a layout node and measures 0 — silently, which is
    // the trap this is here to keep closed. No flush() first: the read lays out.
    const hostRect = hostCanvas.getBoundingClientRect();
    assert(hostRect.width === SIZE && hostRect.height === SIZE,
        `C++ appendChild put the canvas into layout (got ${hostRect.width}x${hostRect.height}, want ${SIZE}x${SIZE})`);

    const beforeHostCtx = __host.sceneContextCount();
    const hostScene = __host.sceneContext(hostCanvas);
    assert(hostScene, 'Engine::createSceneContext returned a scene graph');
    assert(__host.sceneContextCount() === beforeHostCtx + 1,
        'the C++ path registered exactly one new scene context');

    // =====================================================================
    // Section 3: equivalence — identical scenes must render identically.
    // =====================================================================
    buildScene(jsScene);
    buildScene(hostScene);
    flush();

    const jsImg = jsScene.captureFrame();
    const hostImg = hostScene.captureFrame();
    assert(jsImg.width === hostImg.width && jsImg.height === hostImg.height,
        `both contexts sized from the element's layout box (${jsImg.width}x${jsImg.height} vs ${hostImg.width}x${hostImg.height})`);

    const jsRed = patchAvg(jsImg, jsImg.width >> 1, jsImg.height >> 1, 4, 0);
    const hostRed = patchAvg(hostImg, hostImg.width >> 1, hostImg.height >> 1, 4, 0);
    assert(jsRed > 150, `the JS scene rendered the red box (r=${jsRed.toFixed(0)})`);
    assert(hostRed > 150, `the C++ scene rendered the red box (r=${hostRed.toFixed(0)})`);
    assert(maxDiff(jsImg, hostImg) <= 2,
        `the two scene contexts render the same pixels (maxDiff=${maxDiff(jsImg, hostImg)})`);

    // =====================================================================
    // Section 4: compositing — the C++ context reaches the page, not just its
    // own render target. captureFrame() reads the scene's FBO directly and
    // would pass even with the FBO callback missing; getPixel() goes through
    // the layer break and Engine::compositeLayers, which is what actually
    // fails when a graph is not wired to its element.
    // =====================================================================
    flush();
    const jsC = centerOf(jsCanvas);
    const hostC = centerOf(hostCanvas);
    const jsPx = getPixel(jsC.x, jsC.y);
    const hostPx = getPixel(hostC.x, hostC.y);

    assert(jsPx.r > 100 && jsPx.r > jsPx.b,
        `the JS scene composites into the page (rgb ${jsPx.r},${jsPx.g},${jsPx.b})`);
    assert(hostPx.r > 100 && hostPx.r > hostPx.b,
        `the C++ scene composites into the page (rgb ${hostPx.r},${hostPx.g},${hostPx.b})`);
    assert(Math.abs(jsPx.r - hostPx.r) <= 4 &&
           Math.abs(jsPx.g - hostPx.g) <= 4 &&
           Math.abs(jsPx.b - hostPx.b) <= 4,
        `both composite to the same colour (js ${jsPx.r},${jsPx.g},${jsPx.b} vs host ${hostPx.r},${hostPx.g},${hostPx.b})`);

    // =====================================================================
    // Section 5: one context per canvas, whichever path asks for it.
    // =====================================================================

    // C++ asking for a context the JS path already built gets that same graph
    // back — not a second registration.
    const n = __host.sceneContextCount();
    const jsSceneViaCpp = __host.sceneContext(jsCanvas);
    assert(jsSceneViaCpp, 'createSceneContext on a JS-built canvas returns a graph');
    assert(__host.sceneContextCount() === n,
        'and does NOT register a second scene context for the same canvas');

    // Same graph, not merely an equivalent one: a node added through the C++
    // handle is visible through the JS one. Blue box in front of the red one.
    jsSceneViaCpp.createMesh({ mesh: 'box', color: 'blue', x: 0, y: 0, z: 2 });
    flush();
    const afterAdd = jsScene.captureFrame();
    const blue = patchAvg(afterAdd, afterAdd.width >> 1, afterAdd.height >> 1, 4, 2);
    const red = patchAvg(afterAdd, afterAdd.width >> 1, afterAdd.height >> 1, 4, 0);
    assert(blue > 150 && red < 90,
        `the C++ handle and the JS handle are the same SceneGraph (r=${red.toFixed(0)} b=${blue.toFixed(0)})`);

    // The JS cache is unchanged by the C++ call: getContext still returns the
    // one context object, per spec.
    assert(jsCanvas.getContext('scene') === jsScene,
        'getContext still returns the same context object it always did');

    // And the reverse: JS asking for a context on a C++-built canvas gets the
    // graph the C++ path already made.
    const m = __host.sceneContextCount();
    const hostSceneViaJs = hostCanvas.getContext('scene');
    assert(hostSceneViaJs, 'getContext("scene") works on a C++-created canvas');
    assert(__host.sceneContextCount() === m,
        'and does NOT register a second scene context either');

    hostSceneViaJs.createMesh({ mesh: 'box', color: 'blue', x: 0, y: 0, z: 2 });
    flush();
    const hostAfter = hostScene.captureFrame();
    const hBlue = patchAvg(hostAfter, hostAfter.width >> 1, hostAfter.height >> 1, 4, 2);
    const hRed = patchAvg(hostAfter, hostAfter.width >> 1, hostAfter.height >> 1, 4, 0);
    assert(hBlue > 150 && hRed < 90,
        `getContext on a C++-created canvas returns that same SceneGraph (r=${hRed.toFixed(0)} b=${hBlue.toFixed(0)})`);

    // =====================================================================
    // Section 6: the C++-built context is reclaimed like any other when its
    // canvas leaves the document — no special-case lifetime.
    // =====================================================================
    const before = __host.sceneContextCount();
    document.body.removeChild(hostCanvas);
    flush();
    assert(__host.sceneContextCount() === before - 1,
        'removing the C++-created canvas prunes its scene context like any other');

    // =====================================================================
    // Section 7: re-attaching a canvas whose context was pruned must build a
    // fresh one, not hand back the dead pointer. Element::sceneGraph() is NOT
    // cleared when a graph is reclaimed, so it alone would say "already has
    // one" here — which is why createSceneContext also checks the engine's own
    // registration before believing it.
    // =====================================================================
    document.body.appendChild(hostCanvas);
    flush();
    const reAdded = __host.sceneContextCount();
    const revived = __host.sceneContext(hostCanvas);
    assert(revived, 're-attached canvas gets a scene context again');
    assert(__host.sceneContextCount() === reAdded + 1,
        'and it is a NEW registration, not the pruned one');

    buildScene(revived);
    flush();
    const revivedImg = revived.captureFrame();
    const revivedRed = patchAvg(revivedImg, revivedImg.width >> 1,
                                revivedImg.height >> 1, 4, 0);
    assert(revivedRed > 150,
        `the revived context renders (r=${revivedRed.toFixed(0)})`);

    document.body.removeChild(hostCanvas);
    document.body.removeChild(jsCanvas);
    flush();

    console.log('host scene context tests passed');
}
