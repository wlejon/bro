// shadowRange, read back as pixels and page draws: a 2 x 2 m card 2 m up
// under a sun straight above, 3 m from the eye. 'all' and 'near'
// shadow the ground under it; 'far' (the cached pages past the sway level
// alone) leaves the ground near the eye lit. A card that comes and goes
// casting 'all' redraws cached pages; casting 'near' it redraws none.

function patch(img, cx, cy, r) {
    const s = [0, 0, 0];
    let n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            for (let c = 0; c < 3; c++) s[c] += img.data[i + c];
            n++;
        }
    }
    return (s[0] + s[1] + s[2]) / (3 * Math.max(1, n));
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

function card() {
    return new Mesh({
        positions: new Float32Array([-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1]),
        indices: new Uint32Array([0, 2, 1, 0, 3, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
    });
}

// The eye close enough that the ground's pixels read the sway level or
// finer (a few millimetres a pixel), where a near caster's shadow lies.
function setup() {
    const s = freshScene(768);
    const scn = s.scene;
    scn.setCamera({ fov: 50, near: 0.1, far: 80, position: [0, 3.5, 3], target: [0, 0, 0.6] });
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient([0.02, 0.02, 0.02]);
    scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 12, color: [0.8, 0.8, 0.8], roughness: 1, castsShadow: false });
    const sun = scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
    sun.castsShadow = true;
    return { s, scn };
}

// Frames until the cached pages are all drawn (none drawn three frames running).
function settle(scn) {
    let quiet = 0;
    for (let i = 0; i < 300 && quiet < 3; i++) {
        scn.captureFrame();
        quiet = scn.cullStats().shadowPagesDrawn === 0 ? quiet + 1 : 0;
    }
    assert(quiet >= 3, 'the shadow pages settle');
}

// The ground under the card (past its +z edge, seen from above it) and open
// ground beside it.
function look(scn) {
    const img = scn.captureFrame();
    const at = (x, y, z) => {
        const p = scn.projectLocal(x, y, z);
        assert(p && !p.behind, `(${x}, ${y}, ${z}) is on screen`);
        return patch(img, Math.round(p.x), Math.round(p.y), 2);
    };
    return { under: at(0, 0, 0.6), open: at(0, 0, 1.6) };
}

const GREY = { color: [0.5, 0.5, 0.5], doubleSided: true, y: 2 };

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);

    for (const range of ['all', 'near', 'far']) {
        const { s, scn } = setup();
        const m = scn.createMesh(Object.assign({ mesh: card(), shadowRange: range }, GREY));
        assert(m.shadowRange === range, `${range}: the option reads back (${m.shadowRange})`);
        settle(scn);
        const v = look(scn);
        assert(v.open > 60, `${range}: the open ground is sunlit (${v.open.toFixed(1)})`);
        if (range === 'far') {
            assert(v.under > v.open * 0.8, `far: the ground near the eye is lit under the card (${v.under.toFixed(1)} vs ${v.open.toFixed(1)})`);
        } else {
            assert(v.under < v.open * 0.35, `${range}: the ground under the card is shadowed (${v.under.toFixed(1)} vs ${v.open.toFixed(1)})`);
        }
        dropScene(s);
    }

    // Coming and going: the cached pages a card's arrival redraws (a still
    // caster taller than it already there, as the scene's depth range).
    for (const range of ['all', 'near']) {
        const { s, scn } = setup();
        scn.createMesh(Object.assign({ mesh: card() }, GREY, { x: -6, y: 4 }));
        settle(scn);
        // (The first arrival is not counted: the overlay it is drawn into
        // makes pages needed that were not, and they are drawn once.)
        let drawn = 0;
        for (let i = 0; i < 4; i++) {
            // (shadowOnly: its own surface would need pages of its own, which
            // are not what the test is after.)
            const m = scn.createMesh(Object.assign({ mesh: card(), shadowRange: range, shadowOnly: true }, GREY));
            scn.captureFrame();
            const a = scn.cullStats();
            m.destroy();
            scn.captureFrame();
            const b = scn.cullStats();
            if (i > 0) drawn += a.shadowPagesDrawn + b.shadowPagesDrawn;
        }
        if (range === 'all') assert(drawn > 0, `all: a card coming and going redraws cached pages (${drawn})`);
        else assert(drawn === 0, `near: a card coming and going redraws no cached page (${drawn})`);
        dropScene(s);
    }

    {
        const { s, scn } = setup();
        const m = scn.createMesh(Object.assign({ mesh: card() }, GREY));
        assert(m.shadowRange === 'all', `the default range is all (${m.shadowRange})`);
        m.shadowRange = 'near';
        assert(m.shadowRange === 'near', 'the accessor sets it');
        const inst = scn.createInstancedMesh({ mesh: card(), shadowRange: 'far',
            instances: new Float32Array([1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1, 0, 1, 1, 1, 1]) });
        assert(inst.shadowRange === 'far', `instanced: the option reads back (${inst.shadowRange})`);
        dropScene(s);
    }
}
