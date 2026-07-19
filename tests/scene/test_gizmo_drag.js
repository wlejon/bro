// bro.gizmo — drag math under scaled and orthographic views.
//
// Two independent bugs made the gizmo hard to use in exactly the views an
// editor spends its time in:
//
//   1. Mouse routing fed unprojectLocal() an offset measured in absolute
//      (post-CSS-transform) screen pixels, while unprojectLocal divides by the
//      canvas's *layout* size. Under a scale(2) the normalized device x ran
//      -1..3 instead of -1..1, so the ray missed: handles could not even be
//      picked, never mind dragged predictably.
//
//   2. screenStableScale() computed world-units-per-pixel as
//      distance * tan(fov/2), which is a perspective identity. Under an
//      orthographic camera that quantity is fixed by the projection's
//      half-height and does not involve distance at all, so dollying an ortho
//      camera — a no-op on screen — scaled the gizmo with it, and zooming
//      (which changes the half-height) failed to resize it. Every pick radius
//      multiplies by that scale, so grab regions swelled until neighbouring
//      axes overlapped and a drag caught the wrong one.
//
// Both are measured through the real input path (mouseDown/mouseMove) rather
// than by calling the math directly, so routing and picking stay covered.

function makeCanvas(cssScale) {
    document.body.innerHTML = '';
    flush();
    const c = document.createElement('canvas');
    c.setAttribute('width', '400');
    c.setAttribute('height', '300');
    c.style.position = 'absolute';
    c.style.left = '0px';
    c.style.top = '0px';
    c.style.width = '400px';
    c.style.height = '300px';
    if (cssScale !== 1) {
        c.style.transform = 'scale(' + cssScale + ')';
        c.style.transformOrigin = '0 0';
    }
    document.body.appendChild(c);
    flush();
    return c;
}

// Attach a recording target and return it. Deltas accumulate so a test can
// compare total world movement for a gesture.
function attachTarget() {
    const sel = { x: 0, y: 0, z: 0, grabbed: false };
    bro.gizmo.attach({
        position: () => [sel.x, sel.y, sel.z],
        orientation: () => [0, 0, 0, 1],
        beginDrag: () => { sel.grabbed = true; },
        translate: (dx, dy, dz) => { sel.x += dx; sel.y += dy; sel.z += dz; },
        rotate: () => {},
        scale: () => {},
        endDrag: () => {},
    });
    flush();
    return sel;
}

// ===========================================================================
// A CSS transform on the canvas must not change what a drag does.
//
// The gesture is expressed in CANVAS coordinates and multiplied into screen
// space by the scale, so both runs are the same gesture as far as the scene
// is concerned and must produce the same world-space delta.
// ===========================================================================
function dragAlongX(cssScale) {
    const c = makeCanvas(cssScale);
    const s = c.getContext('scene');
    if (!s) return null;
    s.setCamera({ fov: 60, near: 0.1, far: 100,
                  position: [0, 0, 8], target: [0, 0, 0], up: [0, 1, 0] });
    flush();
    bro.gizmo.setMode('translate');
    const sel = attachTarget();

    const y = 150 * cssScale;
    mouseMove(240 * cssScale, y);
    mouseDown(240 * cssScale, y);
    mouseMove(280 * cssScale, y);
    mouseUp(280 * cssScale, y);
    flush();
    return sel;
}

const plain = dragAlongX(1);
if (!plain) {
    console.log('scene context not available (no GPU)');
} else {
    assert(plain.grabbed, 'the +X handle was grabbed in an unscaled view');
    assert(plain.x > 0.1, 'the unscaled drag moved the target, dx=' + plain.x);

    for (const scale of [2, 0.5, 1.75]) {
        const scaled = dragAlongX(scale);
        assert(scaled.grabbed,
               'the +X handle was grabbed under CSS scale(' + scale + ')');
        // Same gesture in canvas space => same world delta, transform aside.
        const drift = Math.abs(scaled.x - plain.x);
        assert(drift < 1e-3,
               'CSS scale(' + scale + ') left the drag unchanged: expected dx=' +
               plain.x + ', got ' + scaled.x);
    }

    // =======================================================================
    // Screen-stable size: the gizmo occupies the same number of screen pixels
    // regardless of camera distance, ortho zoom, or projection mode.
    //
    // Measured by walking right from the pivot and recording the last pixel
    // that still hovers the +X handle — i.e. the handle's on-screen extent.
    // =======================================================================
    function handleExtent(camera) {
        const c = makeCanvas(1);
        const s = c.getContext('scene');
        if (!s) return -1;
        s.setCamera(camera);
        flush();
        bro.gizmo.setMode('translate');
        attachTarget();

        let last = -1;
        for (let px = 200; px < 399; px++) {
            mouseMove(px, 150);
            if (bro.gizmo.hovered === 'x') last = px;
        }
        return last < 0 ? -1 : last - 200;
    }

    const ortho = (dist, size) => ({
        mode: 'orthographic', size: size, near: 0.1, far: 1000,
        position: [0, 0, dist], target: [0, 0, 0], up: [0, 1, 0],
    });
    const persp = (dist) => ({
        fov: 60, near: 0.1, far: 1000,
        position: [0, 0, dist], target: [0, 0, 0], up: [0, 1, 0],
    });

    // Dollying an orthographic camera changes nothing on screen, so it must
    // not change the gizmo's size. This is what regressed: the extent scaled
    // linearly with distance until it swallowed the viewport.
    const o1 = handleExtent(ortho(1, 10));
    assert(o1 > 0, 'the +X handle is pickable under an ortho camera');
    for (const dist of [2, 3, 8, 40]) {
        const oN = handleExtent(ortho(dist, 10));
        assert(Math.abs(oN - o1) <= 2,
               'ortho gizmo size is independent of camera distance: dist=1 gave ' +
               o1 + 'px, dist=' + dist + ' gave ' + oN + 'px');
    }

    // Zooming an ortho camera (changing `size`) must likewise keep it stable.
    for (const size of [2, 5, 20, 50]) {
        const oz = handleExtent(ortho(5, size));
        assert(Math.abs(oz - o1) <= 2,
               'ortho gizmo size is independent of zoom: size=10 gave ' + o1 +
               'px, size=' + size + ' gave ' + oz + 'px');
    }

    // Perspective was already stable; assert it stayed that way, and that the
    // two projection modes agree — they are both targeting the same pixel size.
    const p1 = handleExtent(persp(5));
    const p2 = handleExtent(persp(15));
    assert(Math.abs(p2 - p1) <= 2,
           'perspective gizmo size is independent of camera distance, ' +
           p1 + 'px vs ' + p2 + 'px');
    assert(Math.abs(p1 - o1) <= 3,
           'ortho and perspective agree on the gizmo screen size, ' +
           o1 + 'px vs ' + p1 + 'px');

    console.log('PASS gizmo drag');
}
