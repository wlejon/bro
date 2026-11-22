// alphaToCoverage, read back as pixels: a card whose fragment hook gives
// alpha 0.5 (and white in a debug view), in front of a surface that does
// not answer the view (dim grey). Under 4x MSAA with alphaToCoverage the
// card covers half of each pixel's samples, so the pixel resolves between
// white and the grey; without it the card cuts at its alphaCutoff (0.25)
// and is wholly white. Single-sampled, alphaToCoverage falls back to the cut.

const HOOK = `
module hook;
import mesh;
import scene;
struct Params { a: vec4<f32> }
@binding(4, 0) var params: uniform<Params>;
pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.alpha = params.a.x;
    if (SCENE_DEBUG(a2c)) { out.debug = vec4<f32>(1.0, 1.0, 1.0, 1.0); }
    return out;
}`;

const LIN = (b) => { const c = b / 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };

function patch(img, cx, cy, r) {
    let s = 0, n = 0;
    for (let y = cy - r; y <= cy + r; y++) {
        for (let x = cx - r; x <= cx + r; x++) {
            const i = (y * img.width + x) * 4;
            s += LIN(img.data[i]) + LIN(img.data[i + 1]) + LIN(img.data[i + 2]);
            n += 3;
        }
    }
    return s / n;
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '128');
cv.setAttribute('height', '128');
document.body.appendChild(cv);
flush();
const scn = cv.getContext('scene');
scn.setCamera({ fov: 50, near: 0.1, far: 50, position: [0, 0, 3], target: [0, 0, 0] });
scn.setDebugView('a2c');
scn.setDither(false);
// Behind the card, answering no view: the dim grey.
const quad = (h, z) => new Mesh({
    positions: new Float32Array([-h, -h, z, h, -h, z, h, h, z, -h, h, z]),
    indices: new Uint32Array([0, 1, 2, 0, 2, 3]),
    normals: new Float32Array([0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1]),
});
scn.createMesh({ data: quad(20, -2), color: [1, 1, 1], castsShadow: false });
const card = scn.createMesh({
    data: quad(1, 0),
    color: [1, 1, 1], alphaCutoff: 0.25, alphaToCoverage: true, castsShadow: false,
});
card.setShader({ module: HOOK, file: 'a2c_test.bk', uniforms: { a: [0.5, 0, 0, 0] } });
assert(card.alphaToCoverage === true, 'the factory option sets alphaToCoverage');

function look() {
    for (let i = 0; i < 6; i++) advanceTime(16);
    return patch(scn.captureFrame(), 64, 64, 12);
}
function background() {
    card.visible = false;
    const v = look();
    card.visible = true;
    return v;
}

scn.setMSAA(4);
const bg = background();
const covered = look();
console.log(`a2c: background ${bg.toFixed(3)}, alpha 0.5 covered ${covered.toFixed(3)}`);
assert(bg > 0.01 && bg < 0.5, `the background is the dim grey (${bg.toFixed(3)})`);
assert(Math.abs(covered - (0.5 + 0.5 * bg)) < 0.12,
    `alpha 0.5 covers half the samples: ${covered.toFixed(3)} against ${(0.5 + 0.5 * bg).toFixed(3)}`);

card.setShaderUniform('a', [0.1, 0, 0, 0]);
const thin = look();
assert(thin < covered - 0.15, `alpha 0.1 (under the cutoff) covers less, not nothing cut: ${thin.toFixed(3)}`);
assert(thin > bg - 0.02, `and no less than the background: ${thin.toFixed(3)}`);
card.setShaderUniform('a', [0.5, 0, 0, 0]);

card.alphaToCoverage = false;
assert(card.alphaToCoverage === false, 'the property clears');
const cut = look();
assert(cut > 0.95, `without it alpha 0.5 passes the 0.25 cutoff whole: ${cut.toFixed(3)}`);

card.alphaToCoverage = true;
scn.setMSAA(0);
const single = look();
assert(single > 0.95, `single-sampled it cuts at the cutoff: ${single.toFixed(3)}`);

console.log('test_alpha_to_coverage: PASS');
