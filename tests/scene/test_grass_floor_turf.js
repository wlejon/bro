// The grass node's forest floor and turf (scene/grass_node.h setFloor,
// vulkan/shaders/scene_ground.glsl). Verifies:
//   - createGrass's `herbs` grow low plants among (and without) the grass:
//     with no grass anywhere, a closed floor's wood sorrel still issues herb
//     cells and draws green leaves; a herb density of 0 grows nothing
//   - `turf` / shadeGround shade a ground mesh as the grass's ground: the
//     sward where the grass grows (green, textured), the litter where the
//     floor is closed (brown, textured), and shadeGround(mesh, false) gives
//     the mesh its own colour back

function patchStats(img, x0, y0, x1, y1) {
    let r = 0, g = 0, b = 0, l = 0, l2 = 0, n = 0;
    for (let y = y0; y < y1; y++)
        for (let x = x0; x < x1; x++) {
            const i = (y * img.width + x) * 4;
            const lum = 0.3 * img.data[i] + 0.59 * img.data[i + 1] + 0.11 * img.data[i + 2];
            r += img.data[i]; g += img.data[i + 1]; b += img.data[i + 2];
            l += lum; l2 += lum * lum; n++;
        }
    const m = l / n;
    return { r: r / n, g: g / n, b: b / n, lum: m, sd: Math.sqrt(Math.max(l2 / n - m * m, 0)) };
}

function diffCount(a, b) {
    let n = 0;
    for (let i = 0; i < a.data.length; i += 4)
        if (Math.abs(a.data[i] - b.data[i]) + Math.abs(a.data[i + 1] - b.data[i + 1]) + Math.abs(a.data[i + 2] - b.data[i + 2]) > 6) n++;
    return n;
}

const S = 192;
const cv = document.createElement('canvas');
cv.setAttribute('width', String(S));
cv.setAttribute('height', String(S));
document.body.appendChild(cv);
flush();
const scn = cv.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
    scn.createLight({ type: 'directional', direction: [0.3, -1, 0.2], color: [1, 1, 1], intensity: 8 });
    const floor = scn.createMesh({ mesh: 'plane', halfW: 30, halfD: 30, color: [0.45, 0.45, 0.45] });
    scn.setCamera({ fov: 55, near: 0.05, far: 200, position: [0, 1.6, 5], target: [0, 0, -1] });
    scn.setWind({ speed: 0, gust: 0 });
    const bare = scn.captureFrame();

    // ---- Low plants on the floor, no grass ----
    const grass = scn.createGrass({ ground: floor, cell: 0.5, density: 0, floor: 1,
                                    herbs: { species: ['woodsorrel'], density: 1 } });
    const plants = scn.captureFrame();
    const st = grass.grassStats();
    const herbCells = (st.herbInstances || []).reduce((a, b) => a + b, 0);
    assert(herbCells > 100, `a closed floor's herbs issue cells with no grass (${(st.herbInstances || []).join('/')})`);
    // (They grow in patches: judge the pixels they cover.)
    let gb = 0, gbBare = 0, covered = 0;
    for (let i = 0; i < plants.data.length; i += 4) {
        const d = Math.abs(plants.data[i] - bare.data[i]) + Math.abs(plants.data[i + 1] - bare.data[i + 1]) +
                  Math.abs(plants.data[i + 2] - bare.data[i + 2]);
        if (d <= 6) continue;
        gb += plants.data[i + 1] - plants.data[i + 2];
        gbBare += bare.data[i + 1] - bare.data[i + 2];
        covered++;
    }
    assert(covered > 200, `low plants are drawn (${covered} px)`);
    assert(gb / covered > gbBare / covered + 3, `they are green (g-b ${(gb / covered).toFixed(1)} vs ${(gbBare / covered).toFixed(1)})`);
    grass.setGround({ ground: floor, cell: 0.5, density: 0, floor: 0, herbs: { species: ['woodsorrel'], density: 0 } });
    const none = scn.captureFrame();
    assert(diffCount(none, bare) < 40, `no herb density and no grass grows nothing (${diffCount(none, bare)} px)`);

    // ---- Turf ----
    // Grass to the left, a closed canopy's floor to the right, shaded on the
    // ground; the camera high enough that the ground shows.
    grass.setGround({ ground: floor, cell: 0.5, density: (x) => (x < 0 ? 1 : 0), floor: (x) => (x < 0 ? 0 : 1), turf: true });
    scn.setCamera({ fov: 55, near: 0.05, far: 300, position: [0, 30, 20], target: [0, 0, -10] });
    const turf = scn.captureFrame();
    const sward = patchStats(turf, 10, 90, 80, 150), litter = patchStats(turf, 112, 90, 182, 150);
    assert(sward.g > sward.b + 8 && sward.g > sward.r * 0.9, `the turf under grass is green (${sward.r.toFixed(0)}, ${sward.g.toFixed(0)}, ${sward.b.toFixed(0)})`);
    // (Seen from above under a high sun the sward shows its blades' shaded
    // depths, sceneSwardLight; the litter lies level in the sun.)
    assert(litter.r > litter.b * 1.3 && litter.lum < sward.lum * 1.6, `the floor under a canopy is brown litter (${litter.r.toFixed(0)}, ${litter.g.toFixed(0)}, ${litter.b.toFixed(0)})`);
    assert(sward.sd > 0.08 * sward.lum && litter.sd > 0.08 * litter.lum,
        `both are textured (sd ${sward.sd.toFixed(2)} of ${sward.lum.toFixed(1)}, ${litter.sd.toFixed(2)} of ${litter.lum.toFixed(1)})`);
    grass.shadeGround(floor, false);
    const plain = scn.captureFrame();
    const back = patchStats(plain, 112, 90, 182, 150);
    assert(Math.abs(back.r - back.b) < 4 && back.sd < sward.sd, `shadeGround(mesh, false) gives the mesh its own colour (${back.r.toFixed(0)}, ${back.b.toFixed(0)}, sd ${back.sd.toFixed(2)})`);
    if (globalThis.__turfShot) screenshot(globalThis.__turfShot);
}
