// shadowOnly, read back as pixels: a card that casts its shadow and draws
// nothing else. The sun is straight above an emissive red 2 x 2 m card at
// 2 m; drawn, the card shows red where it projects and shadows the ground
// under it; shadowOnly, the ground shows through grey where the card was and
// is still shadowed under it. Static and instanced.

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
    return s.map((v) => (n ? v / n : 0));
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

function at(scn, img, x, y, z) {
    const p = scn.projectLocal(x, y, z);
    assert(p && !p.behind, `(${x}, ${y}, ${z}) is on screen`);
    return patch(img, Math.round(p.x), Math.round(p.y), 2);
}

function card() {
    return new Mesh({
        positions: new Float32Array([-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1]),
        indices: new Uint32Array([0, 2, 1, 0, 3, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
    });
}

function setup() {
    const s = freshScene(192);
    const scn = s.scene;
    scn.setCamera({ fov: 50, near: 0.1, far: 80, position: [0, 9, 9], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient([0.02, 0.02, 0.02]);
    scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 12, color: [0.8, 0.8, 0.8], roughness: 1, castsShadow: false });
    const sun = scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
    sun.castsShadow = true;
    return { s, scn };
}

// The card's centre on screen, the ground under it (seen past its +z edge's
// shadow, from in front: the card hides the ground right under it), and
// open ground.
function look(scn, what) {
    const img = scn.captureFrame();
    return {
        card: at(scn, img, 0, 2, 0),
        under: at(scn, img, 0, 0, 0.6),
        open: at(scn, img, -4, 0, 0),
        what,
    };
}

function expectDrawn(v) {
    assert(v.card[0] > 120 && v.card[1] < 60, `${v.what}: the card shows red (${v.card.map((c) => c.toFixed(0))})`);
}

function expectShadowOnly(v) {
    const open = (v.open[0] + v.open[1] + v.open[2]) / 3;
    const under = (v.under[0] + v.under[1] + v.under[2]) / 3;
    assert(open > 60, `${v.what}: the open ground is sunlit (${open.toFixed(1)})`);
    assert(Math.abs(v.card[0] - v.card[1]) < 20, `${v.what}: no red where the card is (${v.card.map((c) => c.toFixed(0))})`);
    assert(under < open * 0.35, `${v.what}: the ground under the card is shadowed (${under.toFixed(1)} vs ${open.toFixed(1)})`);
}

const RED = { color: [1, 0, 0], emissive: 2, emissiveColor: [1, 0, 0] };

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);

    {
        const { s, scn } = setup();
        const m = scn.createMesh(Object.assign({ mesh: card(), twoSided: true, y: 2 }, RED));
        expectDrawn(look(scn, 'static, drawn'));
        m.shadowOnly = true;
        assert(m.shadowOnly === true, 'static: shadowOnly reads back');
        expectShadowOnly(look(scn, 'static, shadowOnly'));
        dropScene(s);
    }

    {
        const { s, scn } = setup();
        scn.createInstancedMesh(Object.assign({ mesh: card(), doubleSided: true, shadowOnly: true,
            instances: new Float32Array([1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1, 0, 1, 1, 1, 1]) }, RED));
        expectShadowOnly(look(scn, 'instanced, shadowOnly option'));
        dropScene(s);
    }
}
