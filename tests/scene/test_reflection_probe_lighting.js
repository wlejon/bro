// Reflection probes, beyond capture and box projection
// (test_reflection_probe.js):
//   - faces are captured LIT: white walls in a room with no light reflect
//     nothing, and a red lamp in the room turns the reflection red (an unlit
//     capture would show the walls' white base colour either way);
//   - overlapping probes: a mesh takes the highest-priority probe whose box
//     holds it, ties going to the smaller box.
// The mirror sphere is metallic with no IBL environment and no ambient, so
// without a probe it is black and any colour it shows comes from a probe.

function freshScene(size) {
    const cv = document.createElement('canvas');
    cv.setAttribute('width', String(size));
    cv.setAttribute('height', String(size));
    document.body.appendChild(cv);
    flush();
    const sc = cv.getContext('scene');
    if (sc) {
        sc.setToneMap({ mode: 'linear', exposure: 1.0, gamma: 1.0 });
        sc.setCamera({ fov: 60, near: 0.1, far: 100, position: [0.8, 0, 3.5], target: [0.8, 0, 0] });
        sc.createLight({ type: 'directional', intensity: 0 });
        sc.setAmbient({ color: [0, 0, 0] });
    }
    return { canvas: cv, scene: sc };
}

function dropScene(s) {
    document.body.removeChild(s.canvas);
    flush();
}

// A closed 10-unit room, every wall made by `wall(...)`, and the mirror
// sphere in it.
function room(sc, opts) {
    const walls = [];
    const wall = (x, y, z, sx, sy, sz) => walls.push(sc.createMesh(Object.assign({
        mesh: Mesh.box(sx, sy, sz), x, y, z,
    }, opts)));
    wall(0, -5, 0, 10.4, 0.2, 10.4);
    wall(0, 5, 0, 10.4, 0.2, 10.4);
    wall(-5, 0, 0, 0.2, 10.4, 10.4);
    wall(5, 0, 0, 0.2, 10.4, 10.4);
    wall(0, 0, -5, 10.4, 10.4, 0.2);
    wall(0, 0, 5, 10.4, 10.4, 0.2);
    sc.createMesh({ mesh: Mesh.sphere(1), x: 0.8, y: 0, z: 0, color: [1, 1, 1, 1], metallic: 1, roughness: 0.05 });
    return walls;
}

function center(img) {
    let r = 0, g = 0, b = 0, n = 0;
    for (let y = 60; y < 68; y++) {
        for (let x = 60; x < 68; x++) {
            const i = (y * img.width + x) * 4;
            r += img.data[i]; g += img.data[i + 1]; b += img.data[i + 2]; n++;
        }
    }
    return { r: r / n, g: g / n, b: b / n };
}
const fmt = (c) => `(${c.r.toFixed(0)},${c.g.toFixed(0)},${c.b.toFixed(0)})`;

const probe = freshScene(32);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const SIZE = 128;

    // =====================================================================
    // Lit capture.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        room(sc, { color: [1, 1, 1, 1], roughness: 1 });
        const p = sc.createReflectionProbe({ size: 10, resolution: 64, updateMode: 'manual' });
        p.capture();
        const dark = center(sc.captureFrame());
        assert(dark.r < 25 && dark.g < 25 && dark.b < 25,
            `an unlit room reflects nothing: ${fmt(dark)}`);

        sc.createLight({ type: 'point', position: [0, 0, 3], color: [1, 0, 0], intensity: 30, range: 20 });
        p.capture();
        const red = center(sc.captureFrame());
        assert(red.r > dark.r + 30 && red.r > red.g + 25,
            `a red lamp in the room reflects red: ${fmt(red)}`);
        dropScene(s);
    }

    // =====================================================================
    // Priority: probe A captures the room red, probe B after it turns green.
    // =====================================================================
    {
        const s = freshScene(SIZE);
        const sc = s.scene;
        const walls = room(sc, { color: [1, 0.1, 0.1, 1], emissive: 2 });
        const a = sc.createReflectionProbe({ size: 8, resolution: 64, updateMode: 'manual' });
        a.capture();
        flush();
        for (const w of walls) w.color = [0.1, 1, 0.1, 1];
        const b = sc.createReflectionProbe({ size: 10, resolution: 64, updateMode: 'manual' });
        b.capture();
        flush();

        const isRed = (c) => c.r > c.g + 30;
        const isGreen = (c) => c.g > c.r + 30;
        const tie = center(sc.captureFrame());
        assert(isRed(tie), `equal priority: the smaller box (red) wins ${fmt(tie)}`);
        b.priority = 1;
        const bWins = center(sc.captureFrame());
        assert(isGreen(bWins), `the higher priority (green) wins over the smaller box ${fmt(bWins)}`);
        a.priority = 2;
        const aWins = center(sc.captureFrame());
        assert(isRed(aWins), `raising the other probe's priority takes it back ${fmt(aWins)}`);
        assert(a.priority === 2 && b.priority === 1, 'priority reads back');
        dropScene(s);
    }

    console.log('test_reflection_probe_lighting: OK');
}
