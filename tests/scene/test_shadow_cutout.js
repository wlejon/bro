// Cutout shadow casters, read back as pixels: a textured card with an alpha
// cutoff casts what the colour pass keeps of it (PassShadow's SHADOW_CUTOUT
// pipelines, shadow.bk), a static mesh and an instanced one;
// and an instanced node's custom vertex hook moves its shadow with it, cut
// or not. The sun is straight above a 2 x 2 m card at 2 m whose base colour
// is clear on its -x half and opaque on its +x half, so the ground under the
// -x half is lit and under the +x half shadowed.

function patchBrightness(img, cx, cy, r) {
    let sum = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            sum += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? sum / n : 0;
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

// Brightness of the ground at world (x, 0, z).
function ground(scn, img, x, z) {
    const p = scn.projectLocal(x, 0, z);
    assert(p && !p.behind, `ground point (${x}, ${z}) is on screen`);
    return patchBrightness(img, Math.round(p.x), Math.round(p.y), 2);
}

// The card: 2 x 2 m in XZ, u along x and v along z.
function card() {
    const m = new Mesh({
        positions: new Float32Array([-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1]),
        indices: new Uint32Array([0, 2, 1, 0, 3, 2]),
        normals: new Float32Array([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0]),
        uvs: new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]),
    });
    return m;
}

// 8 x 8, clear where u < 0.5 and opaque white past it.
function halfTexture() {
    const W = 8, H = 8;
    const data = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const i = (y * W + x) * 4;
            data[i] = data[i + 1] = data[i + 2] = 255;
            data[i + 3] = x < W / 2 ? 0 : 255;
        }
    }
    return { width: W, height: H, data };
}

// One instance at (x, y, z), its tint white.
function oneInstance(x, y, z) {
    return new Float32Array([1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 1, 1, 1, 1]);
}

function setup() {
    const s = freshScene(192);
    const scn = s.scene;
    // From the side, so the ground under the card is in view past it.
    scn.setCamera({ fov: 50, near: 0.1, far: 80, position: [0, 9, 9], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient([0.02, 0.02, 0.02]);
    scn.createMesh({ mesh: 'plane', halfW: 12, halfD: 12, color: [0.8, 0.8, 0.8], roughness: 1, castsShadow: false });
    const sun = scn.createLight({ type: 'directional', direction: [0, -1, 0], intensity: 3 });
    sun.castsShadow = true;
    return { s, scn };
}

// The ground under each quarter of a card centred at (cx, cz), against the
// open ground: the clear (-x) half's and the opaque (+x) half's.
function halves(scn, img, cx, cz) {
    const open = ground(scn, img, cx - 4, cz);
    const clear = [ground(scn, img, cx - 0.5, cz - 0.5), ground(scn, img, cx - 0.5, cz + 0.5)];
    const opaque = [ground(scn, img, cx + 0.5, cz - 0.5), ground(scn, img, cx + 0.5, cz + 0.5)];
    return { open, clear, opaque };
}

function expectCut(h, what) {
    assert(h.open > 60, `${what}: the open ground is sunlit (${h.open.toFixed(1)})`);
    for (const c of h.clear)
        assert(c > h.open * 0.9, `${what}: under the clear half is lit (${c.toFixed(1)} vs ${h.open.toFixed(1)})`);
    for (const o of h.opaque)
        assert(o < h.open * 0.35, `${what}: under the opaque half is shadowed (${o.toFixed(1)} vs ${h.open.toFixed(1)})`);
}

function expectWhole(h, what) {
    assert(h.open > 60, `${what}: the open ground is sunlit (${h.open.toFixed(1)})`);
    for (const v of h.clear.concat(h.opaque))
        assert(v < h.open * 0.35, `${what}: the whole card shadows (${v.toFixed(1)} vs ${h.open.toFixed(1)})`);
}

// Moves the card 3 m along +x in its own space (before the instance rows).
const SHIFT = `
module hook;
import mesh;
pub fn vertex(v: mesh::Vertex) -> mesh::Vertex {
    var out = v;
    out.pos.x += 3.0;
    return out;
}`;

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);

    // ---- A static card: cut with a cutoff, whole without --------------------
    {
        const { s, scn } = setup();
        const m = scn.createMesh({ mesh: card(), texture: halfTexture(), alphaCutoff: 0.5, twoSided: true, y: 2 });
        expectCut(halves(scn, scn.captureFrame(), 0, 0), 'static cutout');
        m.alphaCutoff = 0;
        expectWhole(halves(scn, scn.captureFrame(), 0, 0), 'static, no cutoff');
        dropScene(s);
    }

    // ---- An instanced card ----------------------------------------------------
    {
        const { s, scn } = setup();
        const m = scn.createInstancedMesh({ mesh: card(), texture: halfTexture(), alphaCutoff: 0.5, doubleSided: true,
                                            instances: oneInstance(0, 2, 0) });
        expectCut(halves(scn, scn.captureFrame(), 0, 0), 'instanced cutout');
        m.alphaCutoff = 0;
        expectWhole(halves(scn, scn.captureFrame(), 0, 0), 'instanced, no cutoff');

        // A custom vertex module moves the card, and its shadow with it.
        m.setShader({ module: SHIFT });
        let h = halves(scn, scn.captureFrame(), 3, 0);
        expectWhole(h, 'instanced custom module');
        const left = halves(scn, scn.captureFrame(), 0, 0);
        for (const v of left.clear.concat(left.opaque))
            assert(v > left.open * 0.9, `instanced custom module: nothing left where the card was (${v.toFixed(1)})`);

        // Cut and moved.
        m.alphaCutoff = 0.5;
        expectCut(halves(scn, scn.captureFrame(), 3, 0), 'instanced custom module, cutout');
        dropScene(s);
    }

    // ---- A static card's custom module, cut -------------------------------------
    {
        const { s, scn } = setup();
        // (Its cullMargin the hook's reach past its bounds, as setCullMargin
        // asks: the shadow pages its shadow can fall in are found by it.)
        const m = scn.createMesh({ mesh: card(), texture: halfTexture(), alphaCutoff: 0.5, twoSided: true, y: 2,
                                   cullMargin: 3 });
        m.setShader({ module: SHIFT });
        expectCut(halves(scn, scn.captureFrame(), 3, 0), 'static custom module, cutout');
        dropScene(s);
    }
}
