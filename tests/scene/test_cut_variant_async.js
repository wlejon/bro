// A drawn node that gains a cut (here an alpha cutoff) needs its hook's cut
// variant, a pipeline of its own built off the frame thread (a window's
// policy, perf.asyncShaders(true)). Until it is in, the node draws with the
// variant it had, never skipped: a city's walls given a cutoff while one
// building is open vanished for the seconds their pipeline took.

const S = 96;
const frames = (n) => { flush(); for (let i = 0; i < n; i++) advanceTime(1000 / 30); };

// (Unique per run, so no earlier process's pipelines are in memory.)
const MODULE = `
module hook;
import mesh;

pub fn fragment(v: mesh::Vertex, s: mesh::Surface) -> mesh::Surface {
    var out = s;
    out.base_color = vec3<f32>(0.0);
    out.roughness = 1.0;
    out.emissive = vec3<f32>(0.0, 1.0, 0.0);
    out.alpha = 1.0;
    return out;
}
`;

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
    scn.setDither(false);
    scn.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 0, 4], target: [0, 0, 0] });
    const box = scn.createMesh({ mesh: 'box', color: '#ffffff' });
    box.setShader({ module: MODULE, file: 'cut_variant.bk' });

    // A hook never drawn before: its first frame skips it (the counter sees
    // a skip at all).
    frames(1);
    const first = scn.cullStats().pipelineSkips;
    assert(typeof first === 'number', 'cullStats() reports pipelineSkips');
    const s = perf.settle({ maxFrames: 1500 });
    assert(s.settled, `the scene settles (${s.pending.join('; ')})`);
    frames(2);
    assert(scn.cullStats().pipelineSkips === 0, 'a settled scene skips nothing');

    // The cutoff: a new variant, built while the old one draws.
    box.alphaCutoff = 0.5;
    let skips = 0;
    for (let i = 0; i < 4; i++) {
        frames(1);
        skips += scn.cullStats().pipelineSkips;
    }
    assert(skips === 0, `a node gaining a cut keeps drawing while its variant builds (${skips} skipped)`);
    // And back: the uncut variant is in, so nothing to build or skip.
    box.alphaCutoff = 0;
    frames(1);
    assert(scn.cullStats().pipelineSkips === 0, 'losing the cut skips nothing');
    console.log(`first frame of the new hook skipped ${first}`);

    perf.asyncShaders(false);
    canvas.remove();
    flush();
}
