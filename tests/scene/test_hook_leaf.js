// A fragment hook's s.leaf, read back as pixels: a two-sided card lit from
// behind (the sun beyond it, the camera in front) is dark as a plain surface
// and lit through as a leaf, the vegetation shading's translucency.

function patch(img, cx, cy, r) {
    let s = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            const i = (y * img.width + x) * 4;
            s += (img.data[i] + img.data[i + 1] + img.data[i + 2]) / 3;
            n++;
        }
    }
    return n ? s / n : 0;
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

// A 2 x 2 m card in XY at the origin, facing +z (the camera).
function card() {
    return new Mesh({
        positions: new Float32Array([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0]),
        indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
        normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
    });
}

const LEAF = (on) => `
module hook;
import mesh;
pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.leaf = ${on ? '1.0' : '0.0'};
    return out;
}`;

const probe = freshScene(64);
if (!probe.scene) {
    missingGpuContext('scene');
} else {
    dropScene(probe);
    const s = freshScene(128);
    const scn = s.scene;
    scn.setCamera({ fov: 40, near: 0.1, far: 50, position: [0, 0, 5], target: [0, 0, 0] });
    scn.setToneMap({ mode: 'linear', gamma: 1.0 });
    scn.setAmbient([0.01, 0.01, 0.01]);
    // The sun behind the card, a little above, shining toward the camera.
    scn.createLight({ type: 'directional', direction: [0, -0.2, 1], intensity: 4 });
    const m = scn.createMesh({ mesh: card(), color: [0.3, 0.6, 0.2], roughness: 0.8, twoSided: true });
    const at = () => {
        const p = scn.projectLocal(0, 0, 0);
        return patch(scn.captureFrame(), Math.round(p.x), Math.round(p.y), 3);
    };
    m.setShader({ module: LEAF(false) });
    const plain = at();
    m.setShader({ module: LEAF(true) });
    const leaf = at();
    assert(leaf > plain * 1.5 + 4, `a backlit card is lit through as a leaf (${leaf.toFixed(1)} vs ${plain.toFixed(1)} plain)`);
    dropScene(s);
}
