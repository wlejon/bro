// Headless's frames skip a draw whose pipeline is still being built, as a
// window's do (its default, perf.asyncShaders(true)); a read of the frame
// (getPixel, frameImage, screenshot: Engine::capturePixels) draws what its
// frame skipped, waiting for those pipelines. Verifies, with a box whose
// fragment module is salted (its program compiles cold) on a ground:
//   - headless's default is the window's policy
//   - a frame skips the box while its pipeline builds (else the subject is
//     untested: SKIP)
//   - a read right after that frame shows the box, as one taken once the
//     scene has settled does

const SALT = (Math.random() * 1e-6).toFixed(9);
const MODULE = `
module hook;
import mesh;
pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.2, 0.3, 0.8) + vec3<f32>(${SALT});
    out.roughness = 0.6;
    return out;
}`;

const S = 128;
assert(perf.asyncShaders() === true, 'headless frames skip what is building, as a window\'s do');
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
canvas.style.cssText = `position:absolute;left:0;top:0;width:${S}px;height:${S}px`;
document.body.appendChild(canvas);
flush();
const scn = canvas.getContext('scene');
if (!scn) {
    missingGpuContext('scene');
} else {
    scn.setToneMap({ mode: 'aces' });
    scn.setDither(false);
    scn.setAmbient({ color: [1, 1, 1], intensity: 0.6 });
    scn.createLight({ type: 'directional', direction: [-0.4, -0.7, -0.5], color: [1, 1, 1], intensity: 2 });
    scn.createMesh({ mesh: 'plane', color: '#5a7040', scale: [40, 1, 40] });
    const box = scn.createMesh({ mesh: 'box', color: 'white', x: 0, y: 1, z: -6, scale: [3, 2, 1] });
    box.setShader({ module: MODULE });
    scn.setCamera({ fov: 60, near: 0.1, far: 500, position: [0, 2, 0], target: [0, 1, -6], up: [0, 1, 0] });

    // advanceTime's next step renders the frame this one flushed.
    advanceTime(1000 / 30);
    advanceTime(1000 / 30);
    const skipping = perf.pendingWork().some((r) => /^pipelines/.test(r));
    const p = getPixel(S / 2, S / 2);
    perf.settle({ maxFrames: 600 });
    const q = getPixel(S / 2, S / 2);
    console.log(`after a skipping frame: ${p.r},${p.g},${p.b}; settled: ${q.r},${q.g},${q.b}; skipped: ${skipping}`);
    assert(q.b > q.r + 50, `the settled picture shows the box (${q.r},${q.g},${q.b})`);
    assert(p.b > p.r + 50, `the read draws the box its frame skipped (${p.r},${p.g},${p.b})`);
    assert(Math.abs(p.r - q.r) <= 3 && Math.abs(p.g - q.g) <= 3 && Math.abs(p.b - q.b) <= 3,
           'and is the settled picture');
    canvas.remove();
    flush();
    if (!skipping) skipTest('no frame skipped a draw: the subject is untested');
    console.log('test_read_draws_skipped: OK');
}
