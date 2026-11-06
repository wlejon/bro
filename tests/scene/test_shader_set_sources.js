// Shader set B given by sources (perf.loadShaderSet / perf.compareShaders
// with { sources }, docs/headless.md "Shader A/B"): an app hook's own file
// and a module given to a hook, by name, are compiled into twinned
// pipelines, so A and B switch with no compile; set A stays as it was.

function centerChannel(img, ch, r) {
    const cx = Math.floor(img.width / 2), cy = Math.floor(img.height / 2);
    let sum = 0, n = 0;
    for (let y = cy - r; y < cy + r; y++) {
        for (let x = cx - r; x < cx + r; x++) { sum += img.data[(y * img.width + x) * 4 + ch]; n++; }
    }
    return sum / n;
}

const cv = document.createElement('canvas');
cv.setAttribute('width', '96');
cv.setAttribute('height', '96');
document.body.appendChild(cv);
flush();
const scene = cv.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
    scene.setToneMap({ mode: 'linear', gamma: 1.0 });
    const TINT = (rgb) => `module ss_tint; pub fn tint() -> vec3<f32> { return vec3<f32>(${rgb}); }`;
    const HOOK = (k) => `
        module hook;
        import mesh;
        import ss_tint;
        pub fn fragment(v: mesh::Vertex, inout s: mesh::Surface) {
            s.base_color = vec3<f32>(0.0);
            s.roughness = 1.0;
            s.emissive = ss_tint::tint() * ${k};
        }
    `;
    const m = scene.createMesh({ mesh: 'box', color: '#ffffff' });
    m.setShaderModule(HOOK('1.0'), 'ss_hook.bk', { ss_tint: TINT('1.0, 0.0, 0.0') });
    const red = (img) => centerChannel(img, 0, 6), green = (img) => centerChannel(img, 1, 6);
    const a0 = scene.captureFrame();
    assert(red(a0) > 200 && green(a0) < 30, `set A renders the hook's red (r=${red(a0)})`);

    // A given module by name: twinned, B draws green, A still red.
    let set = perf.loadShaderSet({ sources: { ss_tint: TINT('0.0, 1.0, 0.0') } });
    assert(set.twinned >= 1 && set.failed.length === 0 && set.sources[0] === 'ss_tint',
        `a given module twins its hook's pipelines: ${JSON.stringify(set)}`);
    perf.useShaderSet('b');
    const b1 = scene.captureFrame();
    assert(green(b1) > 200 && red(b1) < 30, `set B draws the given module's green (g=${green(b1)}, r=${red(b1)})`);
    perf.useShaderSet('a');
    const a1 = scene.captureFrame();
    assert(red(a1) > 200 && green(a1) < 30, `set A is unchanged (r=${red(a1)})`);

    // The hook's own file by name.
    set = perf.loadShaderSet({ sources: { 'ss_hook.bk': HOOK('0.0') } });
    assert(set.twinned >= 1, `the hook's file twins its pipelines: ${JSON.stringify(set)}`);
    perf.useShaderSet('b');
    const b2 = scene.captureFrame();
    assert(red(b2) < 30, `set B draws the hook's new source (r=${red(b2)})`);
    perf.useShaderSet('a');

    // compareShaders takes the sources as its B; an unchanged copy differs nothing.
    const r = perf.compareShaders({ b: { sources: { 'ss_hook.bk': HOOK('1.0') } }, frames: 6, warm: 1 });
    assert(r.set.differing === 0 && r.set.twinned === 0, `an identical source twins nothing: ${JSON.stringify(r.set)}`);
    assert(r.diff && r.diff.frame && typeof r.diff.frame.ms === 'number', 'compareShaders measured A and B');
    let threw = false;
    try { perf.loadShaderSet({ sources: { x: 1 } }); } catch (e) { threw = true; }
    assert(threw, 'a source that is not a string throws');
    console.log('shader set sources OK');
}
