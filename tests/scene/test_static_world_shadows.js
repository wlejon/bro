// Verifies that world-space static InstancedMeshNode batches keep cached
// shadow pages (shadowPagesDynamic === 0) as the camera translates across a
// large world without re-calling setInstances().

const cv = document.createElement('canvas');
cv.setAttribute('width', '256');
cv.setAttribute('height', '256');
document.body.appendChild(cv);
flush();

const scn = cv.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setCamera({ fov: 55, near: 0.5, far: 3000, position: [0, 35, 120], target: [0, 5, 0] });

    const sun = scn.createLight({ type: 'directional', direction: [-0.35, -1.0, -0.25], color: [1, 0.97, 0.92], intensity: 3.0 });
    sun.castsShadow = true;

    // Ground plane across +/- 1500m
    scn.createMesh({ mesh: Mesh.plane(3000, 3000), color: [0.45, 0.45, 0.42], castsShadow: false, y: 0 });

    // Static world-space instanced building proxies (20x20 = 400 buildings)
    const count = 400;
    const rows = new Float32Array(count * 16);
    let k = 0;
    for (let gz = -10; gz < 10; gz++) {
        for (let gx = -10; gx < 10; gx++) {
            const x = gx * 120 + ((gx * 37) % 25);
            const z = gz * 120 + ((gz * 53) % 25);
            const h = 15 + ((Math.abs(gx * 7 + gz * 13) % 6) * 10);
            const w = 14 + (Math.abs(gx + gz) % 3) * 4;
            const o = k * 16;
            rows[o + 0] = w; rows[o + 5] = h; rows[o + 10] = w; rows[o + 15] = 1;
            rows[o + 12] = x; rows[o + 13] = h * 0.5; rows[o + 14] = z;
            k++;
        }
    }
    const batch = scn.createInstancedMesh({ mesh: Mesh.box(1, 1, 1), color: [0.8, 0.78, 0.74] });
    batch.castsShadow = true;
    batch.setInstances(rows);

    // Let shadow pages settle while stationary
    let settled = false;
    for (let i = 0; i < 12; i++) {
        scn.captureFrame();
        const st = scn.cullStats();
        if (i >= 3 && st.shadowPagesWanted === 0 && st.shadowPagesDrawn === 0) {
            settled = true;
            break;
        }
    }
    assert(settled, 'static world building shadow pages settle');
    assert(scn.cullStats().shadowPagesResident > 0, `shadow pages resident (${scn.cullStats().shadowPagesResident})`);
    assert(scn.cullStats().shadowPagesDynamic === 0, 'stationary instanced buildings report shadowPagesDynamic === 0');

    // Move camera across the scene without mutating instances
    for (let step = 1; step <= 8; step++) {
        const zPos = 120 - step * 25;
        scn.setCamera({ fov: 55, near: 0.5, far: 3000, position: [step * 10, 35, zPos], target: [step * 10, 5, zPos - 120] });
        scn.captureFrame();
        const st = scn.cullStats();
        assert(st.shadowPagesDynamic === 0, `camera step ${step} keeps shadowPagesDynamic === 0 (got ${st.shadowPagesDynamic})`);
    }

    document.body.removeChild(cv);
}
