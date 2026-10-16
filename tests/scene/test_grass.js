// scene.createGrass: procedural blades over a baked field (scene/grass_node.h,
// vulkan/shaders/scene_grass.glsl). Verifies:
//   - the node, its params round trip and the field bake (sampleHeights of a
//     ground mesh, grassHeightAt, a density function)
//   - blades are drawn: ring instances in grassStats, a grassy floor greener
//     than the bare one; a density of 0 grows nothing
//   - grass is left out of raycasts
//   - a calm wind keeps the frame still; a wind moves it
//   - movers attach and detach; a push bends the blades it covers

function patchAvg(img, x0, y0, x1, y1, ch) {
    let sum = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    return n ? sum / n : 0;
}

function diffCount(a, b) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]) > 6) n++;
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

const S = 192;
const s = freshScene(S);
if (!s.scene) {
    missingGpuContext('scene');
} else {
    const scn = s.scene;
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 3 });
    const floor = scn.createMesh({ mesh: 'plane', halfW: 20, halfD: 20, color: [0.45, 0.45, 0.45] });
    floor.position = [0, 0.5, 0];
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 2.2, 6], target: [0, 0.5, -2] });
    scn.setWind({ speed: 0, gust: 0 });

    const bare = scn.captureFrame();

    // ---- The bake ----
    const hs = floor.sampleHeights(-25, 0, 1, 51, 1);
    assert(hs.length === 51, `sampleHeights returns cols x rows (${hs.length})`);
    assert(Number.isNaN(hs[0]) && Math.abs(hs[25] - 0.5) < 1e-4,
        `sampleHeights: NaN off the mesh, its top inside (${hs[0]}, ${hs[25]})`);

    const grass = scn.createGrass({ ground: floor, cell: 0.5, height: 0.35, dry: (x) => x > 0 ? 1 : 0 });
    assert(grass && typeof grass.grassParams === 'function', 'createGrass returns a node');
    const p = grass.grassParams();
    assert(Math.abs(p.height - 0.35) < 1e-5, `height param round trips (${p.height})`);
    assert(p.lush.length === 3 && p.dryColor.length === 3, 'colours read back as [r, g, b]');
    grass.setGrass({ lush: [0.1, 0.2, 0.03], wind: 1.2 });
    assert(Math.abs(grass.grassParams().wind - 1.2) < 1e-5 && Math.abs(grass.grassParams().height - 0.35) < 1e-5,
        'setGrass changes what it names and keeps the rest');
    assert(Math.abs(grass.grassHeightAt(3.2, -4.1) - 0.5) < 1e-4, 'grassHeightAt reads the baked ground');
    assert(floor.grassParams() === undefined, 'grass members answer undefined on a plain mesh');

    // ---- Drawn ----
    const lush = scn.captureFrame();
    const st = grass.grassStats();
    assert(st && st.instances[0] > 1000 && st.fieldCols === 81,
        `rings issue instances (${st && st.instances.join('/')}), field ${st && st.fieldCols}`);
    // Left half lush, right half sun-dried: more green on the left than the bare floor.
    const gBare = patchAvg(bare, 10, 120, 80, 180, 1) - patchAvg(bare, 10, 120, 80, 180, 2);
    const gLush = patchAvg(lush, 10, 120, 80, 180, 1) - patchAvg(lush, 10, 120, 80, 180, 2);
    assert(gLush > gBare + 6, `the lush half reads green (g-b ${gLush.toFixed(1)} vs bare ${gBare.toFixed(1)})`);
    const rDry = patchAvg(lush, 112, 120, 182, 180, 0) - patchAvg(lush, 112, 120, 182, 180, 2);
    const rLush = patchAvg(lush, 10, 120, 80, 180, 0) - patchAvg(lush, 10, 120, 80, 180, 2);
    assert(rDry > rLush, `the dry half is warmer (r-b ${rDry.toFixed(1)} vs ${rLush.toFixed(1)})`);

    // Not hit by rays.
    const hit = scn.raycast([0, 5, 2], [0, -1, 0]);
    assert(hit && Math.abs(hit.point[1] - 0.5) < 0.01, `raycasts pass through grass to the floor (${hit && hit.point[1]})`);

    // ---- Still in a calm, moving in a wind ----
    advanceTime(200);
    const calm2 = scn.captureFrame();
    assert(diffCount(lush, calm2) === 0, `calm: nothing moves (${diffCount(lush, calm2)} px)`);
    scn.setWind({ direction: [1, 0, 0], speed: 5, gust: 1.5 });
    const w1 = scn.captureFrame();
    advanceTime(300);
    const w2 = scn.captureFrame();
    assert(diffCount(w1, w2) > 200, `wind: the blades move (${diffCount(w1, w2)} px)`);
    scn.setWind({ speed: 0, gust: 0 });

    // ---- Movers and pushes ----
    const ball = scn.createMesh({ mesh: 'sphere', radius: 0.3, color: [0.8, 0.2, 0.2] });
    ball.position = [0, 0.8, 0];
    grass.addMover(ball, { radius: 0.8, trample: 0.5 });
    assert(grass.grassStats().movers === 1, 'addMover tracks the node');
    grass.removeMover(ball);
    assert(grass.grassStats().movers === 0, 'removeMover stops');
    ball.visible = false;
    advanceTime(100);
    const before = scn.captureFrame();
    grass.push(0, 1, { radius: 1.5, vx: 3, strength: 1, trample: 1 });
    advanceTime(16);
    const after = scn.captureFrame();
    assert(diffCount(before, after) > 100, `a push bends the blades (${diffCount(before, after)} px)`);

    // ---- No density, no grass ----
    grass.setGround({ ground: floor, cell: 0.5, density: 0 });
    const none = scn.captureFrame();
    assert(diffCount(none, bare) < 40, `density 0 grows nothing (${diffCount(none, bare)} px differ from bare)`);

    if (globalThis.__grassShot) screenshot(globalThis.__grassShot);
}
