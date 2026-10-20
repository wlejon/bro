// perf.gpuPasses(): the last timed scene render's GPU milliseconds pass by
// pass. Nothing is stamped before the first call, which turns the marks on
// and so finds none; a render after it reads back its passes, each a
// non-negative time, ending in `rest`.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '160');
canvas.setAttribute('height', '120');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (!scene) {
    missingGpuContext('scene');
} else {
    scene.setCamera({ fov: 60, near: 0.1, far: 100, position: [0, 1, 4], target: [0, 0, 0], up: [0, 1, 0] });
    scene.createLight({ type: 'directional', intensity: 2, castShadow: true });
    scene.createMesh({ mesh: Mesh.box(1, 1, 1), color: [0.8, 0.3, 0.2, 1] });
    scene.createMesh({ mesh: Mesh.box(10, 0.1, 10), color: [0.5, 0.5, 0.5, 1], y: -0.6 });

    flush();
    const timed = perf.gpuFrameMs() >= 0;   // a queue that writes timestamps
    const first = perf.gpuPasses();
    assert(typeof first === 'object' && Object.keys(first).length === 0,
           'the first call finds no passes (nothing was stamped before it)');

    flush();
    const passes = perf.gpuPasses();
    if (!timed) {
        assert(Object.keys(passes).length === 0, 'no timestamps, no passes');
    } else {
        const names = Object.keys(passes).filter(k => k !== 'compute');
        assert(names.length > 1, `a render's passes are kept (got ${JSON.stringify(passes)})`);
        assert(names[names.length - 1] === 'rest', 'the passes end in rest');
        assert(names.includes('setup'), 'the frame setup is a pass');
        for (const k of names)
            assert(typeof passes[k] === 'number' && passes[k] >= 0, `pass ${k} has a time (${passes[k]})`);
        if (passes.compute !== undefined) {
            assert(typeof passes.compute === 'object', 'the compute stream is an object of its stages');
            for (const [k, v] of Object.entries(passes.compute))
                assert(typeof v === 'number' && v >= 0, `compute stage ${k} has a time (${v})`);
        }
        // Read again with no render between: the same render's passes.
        const again = perf.gpuPasses();
        assert(JSON.stringify(Object.keys(again)) === JSON.stringify(Object.keys(passes)),
               'a second read without a render returns the same passes');
    }
}
