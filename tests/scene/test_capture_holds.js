// A captureFrame holds everything it draws under a window's policy too
// (perf.asyncShaders(true): frames skip a draw whose pipeline is still being
// built). The capture is read back at once, so it waits for those pipelines
// as headless's frames do (SceneGraph::renderForRead, scene_vk_target_format.h
// pipeline_async::Hold): a page's one-off capture in a window is the picture
// headless takes. Verifies, with a box whose fragment module is salted (its
// program compiles cold) on a ground:
//   - a window's frame skips the box while its pipeline builds (else the
//     subject is untested: SKIP)
//   - a capture taken right after that frame draws the box: it matches one
//     taken once the scene has settled

const SALT = (Math.random() * 1e-6).toFixed(9);
const MODULE = `
module hook;
import mesh;
pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.75, 0.3, 0.15) + vec3<f32>(${SALT});
    out.roughness = 0.6;
    return out;
}`;

const S = 128;
perf.asyncShaders(true);
const canvas = document.createElement('canvas');
canvas.setAttribute('width', String(S));
canvas.setAttribute('height', String(S));
document.body.appendChild(canvas);
flush();
const scn = canvas.getContext('scene');
if (!scn) {
    perf.asyncShaders(false);
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

    // The box's colour at the view's centre (its red well over its blue).
    const boxPixel = (img) => {
        const o = 4 * ((S / 2) * S + S / 2);
        return [img.data[o], img.data[o + 1], img.data[o + 2]];
    };
    // A frame of the window's: what it skipped shows as pending pipelines.
    advanceTime(1000 / 30);
    const skipping = perf.pendingWork().some((r) => /^pipelines/.test(r));
    const first = scn.captureFrame();
    perf.settle({ maxFrames: 600 });
    const settled = scn.captureFrame();
    perf.asyncShaders(false);
    const a = boxPixel(first), b = boxPixel(settled);
    console.log(`after a skipping frame: ${a}; settled: ${b}; the frame skipped: ${skipping}`);
    assert(b[0] > b[2] + 50, `the settled picture shows the box (centre ${b})`);
    assert(a[0] > a[2] + 50, `the capture draws the box (centre ${a})`);
    assert(a.every((v, i) => Math.abs(v - b[i]) <= 3), `and is the settled picture (${a} / ${b})`);
    canvas.remove();
    flush();
    if (!skipping) skipTest('no frame skipped a draw: the subject is untested');
    console.log('test_capture_holds: OK');
}
