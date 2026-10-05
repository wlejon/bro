// The instanced draw modes beyond plain instance rows, by their pixels:
//   - branch tubes (setTubeSegments): walls built in the vertex shader from
//     segment records alone, and their shadow (shadow_tube.vert);
//   - foliage scatter (setScatterSegments): one leaf per instance, placed in
//     the vertex shader along its segment, deterministically;
//   - static batching (staticBatch): every instance merged into one mesh with
//     its tint baked in, pixel-identical to the instanced draw, and rebaked
//     when an instance moves.

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

function px(img, x, y) {
    const i = (y * img.width + x) * 4;
    return [img.data[i], img.data[i + 1], img.data[i + 2], img.data[i + 3]];
}

// Mean of one channel over a box.
function boxMean(img, ch, x0, y0, x1, y1) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    }
    return sum / n;
}

function diffCount(a, b, tol) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4) {
        for (let c = 0; c < 4; c++) {
            if (Math.abs(a.data[i + c] - b.data[i + c]) > tol) { n++; break; }
        }
    }
    return n;
}

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Tubes: an unlit red vertical tube, radius 0.3, down the view centre.
    // The camera at z = 6 with a 60° fov shows ~18.5 px per unit.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 6], target: [0, 0, 0] });
        const tube = sc.createInstancedMesh({ color: [1, 0, 0, 1], unlit: true });
        tube.setTubeSegments({
            segments: new Float32Array([0, -1.5, 0, 0.3, 0, 1.5, 0, 0.3]),
            sides: 8, boundsMin: [-0.3, -1.5, -0.3], boundsMax: [0.3, 1.5, 0.3],
        });
        assert(tube.isTube === true, 'node is in tube mode');
        const img = sc.captureFrame();
        const c = px(img, 64, 64);
        assert(c[0] > 200 && c[1] < 30 && c[2] < 30 && c[3] > 200,
            `tube wall covers the centre in red: ${c}`);
        const side = px(img, 64 + 15, 64);   // 0.8 units out: past the wall
        assert(side[3] === 0, `outside the tube radius stays clear: ${side}`);
        const above = px(img, 64, 64 - 40);  // 2.2 units up: past the end
        assert(above[3] === 0, `above the tube's end stays clear: ${above}`);
        dropScene(s);
    }

    // =====================================================================
    // Tube shadow: a horizontal tube along z, 1.5 above a white ground,
    // under a sun travelling +x and down. Seen from straight above, its
    // shadow falls 1.5 units to the right; the ground as far to the left is
    // lit.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 8, 0], target: [0, 0, 0], up: [0, 0, -1] });
        sc.createMesh({ mesh: 'plane', halfW: 6, halfD: 6, color: 'white', castsShadow: false });
        const sun = sc.createLight({ type: 'directional', direction: [1, -1, 0], color: [1, 1, 1], intensity: 2 });
        sun.castsShadow = true;
        const tube = sc.createInstancedMesh({ color: [1, 1, 1, 1] });
        tube.setTubeSegments({
            segments: new Float32Array([0, 1.5, -3, 0.25, 0, 1.5, 3, 0.25]),
            sides: 8, boundsMin: [-0.25, 1.25, -3], boundsMax: [0.25, 1.75, 3],
        });
        const img = sc.captureFrame();
        // 8 units up with a 60° fov: ~13.9 px per unit, so 1.5 units = 21 px.
        const shadowed = boxMean(img, 0, 64 + 17, 50, 64 + 25, 78);
        const lit = boxMean(img, 0, 64 - 25, 50, 64 - 17, 78);
        assert(lit > 60, `the ground left of the tube is lit: ${lit.toFixed(0)}`);
        assert(shadowed < lit * 0.6,
            `the tube shadows the ground to its right: ${shadowed.toFixed(0)} vs lit ${lit.toFixed(0)}`);

        tube.castsShadow = false;
        const noShadow = sc.captureFrame();
        const after = boxMean(noShadow, 0, 64 + 17, 50, 64 + 25, 78);
        assert(after > lit * 0.9, `with castsShadow off the ground there is lit: ${after.toFixed(0)}`);
        dropScene(s);
    }

    // =====================================================================
    // Scatter: 200 unlit green leaf cards along a vertical branch segment.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 6], target: [0, 0, 0] });
        const leaves = sc.createInstancedMesh({ mesh: Mesh.plane(0.08, 0.15), color: [0, 1, 0, 1], unlit: true,
                                                doubleSided: true });
        const count = 200;
        leaves.setScatterSegments({
            segments: new Float32Array([0, -1.5, 0, 0.05, 0, 3, 0, count]),
            instSeg: new Float32Array(count),   // every leaf on segment 0
            seed: 7, upBias: 0.3, tiltJitter: 0.3, rollJitter: 0.2, baseScale: 1.0, scaleJitter: 0.2,
            boundsMin: [-0.5, -2, -0.5], boundsMax: [0.5, 2, 0.5],
        });
        assert(leaves.isScatter === true, 'node is in scatter mode');
        const img = sc.captureFrame();
        let green = 0, outside = 0, top = 0, bottom = 0;
        for (let y = 0; y < SIZE; y++) {
            for (let x = 0; x < SIZE; x++) {
                const p = px(img, x, y);
                if (!(p[1] > 150 && p[0] < 40 && p[3] > 0)) continue;
                green++;
                if (Math.abs(x - 64) > 20) outside++;   // ~1.1 units: past any leaf's reach
                if (y < 64) top++; else bottom++;
            }
        }
        assert(green > 150, `the scatter draws its leaves: ${green} green pixels`);
        assert(outside === 0, `leaves stay on their branch: ${outside} green pixels far off it`);
        assert(top > green * 0.2 && bottom > green * 0.2,
            `leaves spread along the whole segment: ${top} above, ${bottom} below`);
        const again = sc.captureFrame();
        assert(diffCount(img, again, 0) === 0, 'leaf placement is deterministic frame to frame');
        dropScene(s);
    }

    // =====================================================================
    // Static batch: red and green tinted boxes. The merged draw matches the
    // instanced one pixel for pixel, and moving an instance rebakes it.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 6], target: [0, 0, 0] });
        const rows = new Float32Array([
            1, 0, 0, -1.5, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1,
            1, 0, 0, 1.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1,
        ]);
        const boxes = sc.createInstancedMesh({ mesh: Mesh.box(), color: [1, 1, 1, 1], instances: rows });
        const instanced = sc.captureFrame();
        boxes.staticBatch = true;
        assert(boxes.staticBatch === true, 'staticBatch reads back');
        const batched = sc.captureFrame();
        const isRed = (p) => p[0] > 40 && p[0] > 4 * p[1] && p[0] > 4 * p[2];
        const isGreen = (p) => p[1] > 40 && p[1] > 4 * p[0] && p[1] > 4 * p[2];
        const left = px(batched, 64 - 28, 64), right = px(batched, 64 + 28, 64);
        assert(isRed(left) && isGreen(right), `batched tints are baked: left ${left}, right ${right}`);
        const nd = diffCount(instanced, batched, 2);
        assert(nd === 0, `the batch renders like the instanced draw (${nd} pixels differ)`);

        // Move the red box to the centre: the batch rebakes.
        boxes.updateInstance(0, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1]);
        const moved = sc.captureFrame();
        assert(isRed(px(moved, 64, 64)), `the moved instance shows at the centre: ${px(moved, 64, 64)}`);
        assert(px(moved, 64 - 28, 64)[3] === 0, 'and has left its old place');
        dropScene(s);
    }

    console.log('test_instanced_modes: OK');
}
